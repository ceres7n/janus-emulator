#include "game_session.hpp"
#include "game_server.hpp"
#include "client_text.hpp"
#include "logger.hpp"
#include <chrono>
#include <cstdlib>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
//  Party quests — retail GameServer (NpcScript 153..167, CPartyManager, CPartyQuestInventory, hAcceptPartyQuest,
//  hInsertPartyQuest, hRewardPartyQuest), SystemServer (CPartyQuestMonster at Party +648) and MasterServer
//  (QuestStore: the Paula procedures USPM_Get_PartyQuestUid / USPM_Get_PartyQuestComCount /
//  USPM_Complete_PartyQuest / USPM_GetList_PartyQuest / USPM_Reward_PartyQuest, statement by statement on
//  party_quest / party_quest_member / char_quest / char_quest_history(_log), sql/018). Every S2C here is
//  VERIFIED in the KR client (sub_6F0960 cases 871 / 873 / 876 / 1192 / 1193 / 1194 / 1195).
//
//  The data (db/party_quest_db.yml, PartyMonQuestInfo): a quest takes its grade 0 only — kill Cnt0 x the party's
//  member count of SrcMonster0 within Time minutes; PMonQuestResult row 0 pays the items, PartyQuestExpTable
//  scales the EXP / TM EXP by the times the character has done it.
//
//  NpcScript (ExecuteNpcScript sub_7E85A0), the quest = the NPC's QuestId[slot]:
//    153..157 CheckPartyQuest (sub_7E1B20): no quest -1; ours and done 2; no party 6; not the master 7; a
//      member outside the master's zone 8; MinLevel above the party's lowest level or MaxLevel below its
//      highest 9; a member busy (a monster quest running +3741 or a party quest entry, sub_869A20) -> S2C 876
//      [u16 n][n x str name] and 5; else 1.
//    158..162 AcceptPartyQuest (sub_7E1CC0): not the master 4; no party 3; the zone 5; the levels 6; busy 2;
//      no message argument -1; else hAcceptPartyQuest (the Master: a PartyQuest row, the members' times done,
//      type 7) -> OnOk (sub_79B920): the zone again, then every member (CPartyManager::AddPartyQuest
//      sub_869CD0) gets the entry {its uid, qid, SrcMonster0, kills 0, target = Cnt0 x members, members, the
//      times done} and the timer (now, 60000 x Time) with S2C 1193 [u32 qid][u16 monster][u16 target][u32 ms]
//      [u8 times], 1194 [u32 ms][u32 ms] and 1195 [u16 0][u16 target][u8 0]; the party's CPartyQuestMonster
//      starts (SYSCMD 5553) with a kill record per member; the NPC says the argument's message (S2C 115).
//    163..167 CompletePartyQuest (sub_7E1F00 -> hRewardPartyQuest sub_7EB9E0): the entry must be done; the
//      row of grade 0 — Type 1: one of its items at random (rand() % count), else all; their weight within
//      MaxWT and as many free places -> else 2; USPM_Reward_PartyQuest -> OnOk (sub_79C470): the bag records,
//      the pet's line 15, EXP = (int)(((members * 0.05 + 0.2) * Exp + (members - 1) * Exp * kills / target)
//      * ratio(times done)) and the TM EXP the same with Tmxp (sub_79CC60 / sub_79CDA0), S2C 873 [u32 EXP]
//      [u32 TM][u16 n][records][u32 qid], Player::UpdatePartyQuestExp (AddExp / AddTMExp without the rates;
//      no level-up -> S2C 0x26 / 285), the entry goes, the NPC says the argument's message. (The guild point
//      of the procedure needs a guild; S2C 874 / 875 go only to a player whose guide flag 9 is 2 — none here.)
//  The kills: the monster's owner (CommonMonster die sub_7296E0 -> sub_7295F0) with an active entry of that
//    monster tells the party (SYSCMD 5557) -> one more on its record and the total (none for a character not
//    in the records, sub_512200) -> every member (msg 21, PartyOrderUpdateQuest -> sub_75E430): its kills and
//    the total, S2C 1195 [u16 total][u16 target][u8 n]{u64 character, u16 kills}; total == target: done, the
//    time left kept — hInsertPartyQuest (USPM_Complete_PartyQuest: the member row, char_quest type 7) and
//    S2C 871 [u32 qid][u8 ReturnMapID != 0]; the party's run ends (sub_511F20). The client's return portal then
//    sends C2S 1040 [u32 9] (monster_quest.cpp handle_monq_return: a done entry -> ReturnMapID in 500 ms).
//  The timer (Player tick sub_75E5B0, not while dead): every second S2C 1194 [u32 left][u32 limit] (the pet's
//    line 16 at 10 s or less); at 0 the entry goes (S2C 1196 has no client case: not sent) and the master
//    stops the party's run (SYSCMD 5589).
//  The master's C2S 7088 (sub_50F9C0, reason 3 — the KR client has no sender of it) and a disbanded party
//    (sub_4F1420, reason 1): every member's entry goes with S2C 1192 [u32 qid][u32 reason] (msg 18,
//    PartyOrderCancelQuest sub_86B2D0).
//  A field change carries the entry (Client::PrepareLeavingZone); the party's OnNotifyEnterZone sends the
//  update again. A login reads the done ones the reward still waits for (USPM_GetList_PartyQuest, sub_7DB730),
//  or takes the party's running one back (PartyOrderRecoverQuest sub_86B810).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr int kSqlParty = 7;         // the SQL quest type of a party quest

// sub_7EB070 (see quest.cpp): the idx-th u16 argument, read from the start.
bool pq_arg(const uint8_t* p, size_t size, int cnt, int idx, int& out)
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

int64_t pq_now()                     // sub_60C740() / 10000: the clock in ms
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// ── the members' side ────────────────────────────────────────────────────────────────────────────────

// sub_869A20's test of a member: a monster quest running (+3741) or a party quest entry (active or done).
bool GameClientSession::pq_busy() const
{
    return mqs_.active || pq_.qid != 0;
}

GameClientSession::PqMember GameClientSession::pq_member() const
{
    PqMember m;
    m.zone  = zone_id_;
    m.level = level_;
    m.busy  = pq_busy();
    m.name  = char_name_;
    return m;
}

void GameClientSession::pq_send_time(int32_t a, int32_t b)          // S2C 1194 (sub_5FA9F0)
{
    PacketBuffer p(static_cast<Opcode>(1194), is_paula());
    p.write<uint32_t>(static_cast<uint32_t>(a));
    p.write<uint32_t>(static_cast<uint32_t>(b));
    send(p);
}

void GameClientSession::pq_send_kills(uint16_t total, uint16_t target, const std::vector<PqRec>& recs)   // S2C 1195
{
    PacketBuffer p(static_cast<Opcode>(1195), is_paula());
    p.write<uint16_t>(total);
    p.write<uint16_t>(target);
    uint8_t n = 0;
    for (const PqRec& r : recs) if (r.uid) ++n;
    p.write<uint8_t>(n);
    for (const PqRec& r : recs) {
        if (!r.uid) continue;
        p.write<uint64_t>(r.uid);
        p.write<uint16_t>(r.kills);
    }
    send(p);
}

// CPartyManager::AddPartyQuest's part for one member (sub_7DB880 + sub_5FA940 + sub_75E3D0 + sub_75E400): false
// when an entry is there already (sub_7DAB20 refuses a used entry).
bool GameClientSession::pq_add(uint64_t uid, int qid, uint8_t members, uint16_t monster, uint16_t target,
                               int64_t start, int32_t limit, uint16_t com)
{
    if (pq_.active() || pq_.qid) {
        LOG_ERROR("CPartyManager::AddPartyQuest - failure add Quest, CHRUID:%u, QUESTID:%d", char_uid_, qid);
        return false;
    }
    if (!server_.quest_db().party(qid)) {
        LOG_ERROR("CPartyQuestInventory::AddQuest - not found questInfo, QuestId:%d", qid);
        return false;
    }
    if (!target) { LOG_ERROR("CPartyQuestInventory::AddQuest - invalid targetCount, QuestId:%d", qid); return false; }
    if (!monster) { LOG_ERROR("CPartyQuestInventory::AddQuest - invalid monsterType, QuestId:%d", qid); return false; }
    if (!limit) { LOG_ERROR("CPartyQuestInventory::AddQuest - invalid limiteTime, QuestId:%d", qid); return false; }
    pq_ = PartyQuestState{};
    pq_.qid      = qid;
    pq_.uid      = uid;
    pq_.monster  = monster;
    pq_.target   = target;
    pq_.members  = members;
    pq_.com      = com;
    pq_.limit_ms = limit;                                    // sub_7DB030
    pq_.start_ms = start;
    pq_.left_ms  = limit;
    {
        PacketBuffer p(static_cast<Opcode>(1193), is_paula());   // sub_5FA940
        p.write<uint32_t>(static_cast<uint32_t>(qid));
        p.write<uint16_t>(monster);
        p.write<uint16_t>(target);
        p.write<uint32_t>(static_cast<uint32_t>(limit));
        p.write<uint8_t>(static_cast<uint8_t>(com));
        send(p);
    }
    pq_send_time(limit, limit);                             // sub_75E3D0
    pq_send_kills(0, target, {});                           // sub_75E400
    LOG_INFO("GameClientSession %u: party quest %d taken (monster %u x%u, %d ms, done %u times)", session_id(), qid,
             monster, target, limit, com);
    return true;
}

// PartyOrderUpdateQuest (sub_86B610 -> sub_75E430): the party's kills; total == target -> done.
void GameClientSession::pq_update(int qid, uint16_t total, std::vector<PqRec> recs)
{
    if (pq_.qid != qid || !pq_.active()) return;
    uint16_t mine = 0;
    for (const PqRec& r : recs) if (r.uid && r.uid == char_uid_) mine = r.kills;
    // sub_7DB100: not timed out and not done -> ours and the total; the target reached -> done, the time left kept
    pq_.kills = mine;
    pq_.total = total;
    if (pq_.total >= pq_.target) {
        pq_.done   = true;
        pq_.record = pq_.left_ms;
    }
    pq_send_kills(total, pq_.target, recs);                 // sub_5FAA70
    if (!pq_.done) return;
    const MonsterQuestDef* d = server_.quest_db().party(qid);
    const bool ret = d && d->return_map != 0;               // sub_7DB530: ReturnMapID
    pq_insert();                                            // hInsertPartyQuest::Send (sub_79DEF0)
    PacketBuffer p(static_cast<Opcode>(871), is_paula());   // sub_5FADD0
    p.write<uint32_t>(static_cast<uint32_t>(qid));
    p.write<uint8_t>(ret ? 1 : 0);
    send(p);
    LOG_INFO("GameClientSession %u: party quest %d done (%u of %u by us, %d ms left)", session_id(), qid, pq_.kills,
             pq_.target, pq_.record);
}

// PartyOrderCancelQuest (sub_86B2D0): S2C 1192 [u32 qid][u32 reason], the entry goes.
void GameClientSession::pq_cancelled(int qid, uint32_t why)
{
    if (pq_.qid != qid) return;
    PacketBuffer p(static_cast<Opcode>(1192), is_paula());  // sub_5FAB80
    p.write<uint32_t>(static_cast<uint32_t>(qid));
    p.write<uint32_t>(why);
    send(p);
    pq_ = PartyQuestState{};
    LOG_INFO("GameClientSession %u: party quest %d cancelled (reason %u)", session_id(), qid, why);
}

// The party's run as this member's GameServer meets it at a zone enter or a login (OnNotifyEnterZone /
// PartyOrderRecoverQuest sub_86B810): the party plays `qid` and we have no entry -> it is made again with the
// time left and S2C 1193 / 1194 / 1195; we hold a running entry the party no longer plays -> it goes (the
// cancel this member missed on the move: S2C 1192 reason 1).
void GameClientSession::pq_party_state(int qid, uint64_t uid, uint8_t members, uint16_t monster, uint16_t target,
                                       int64_t start, int32_t limit, uint16_t com)
{
    if (qid) {
        if (pq_.qid == qid) return;
        if (pq_.qid) return;                                // (another party's done entry waits for its reward)
        if (!pq_add(uid, qid, members, monster, target, start, limit, com)) return;
        pq_.left_ms = static_cast<int32_t>(limit - (pq_now() - start));   // sub_7DB350
        if (pq_.left_ms <= 0) pq_.left_ms = 0;
        pq_send_time(pq_.left_ms, limit);
        return;
    }
    if (pq_.qid && !pq_.done) pq_cancelled(pq_.qid, 1);
}

// Player::OnProcessState's tick (sub_75E5B0; not in the dead state).
void GameClientSession::pq_tick(int elapsed_ms)
{
    if (!pq_.active()) return;
    pq_.acc_ms += elapsed_ms;                               // sub_7DB1E0: the time left by the clock
    pq_.left_ms = static_cast<int32_t>(pq_.limit_ms - (pq_now() - pq_.start_ms));
    if (pq_.left_ms <= 0) pq_.left_ms = 0;
    if (pq_.acc_ms >= 1000) {
        pq_.acc_ms = 0;
        if (pq_.left_ms <= 10000) pet_speech(16);           // our pet's line
        pq_send_time(pq_.left_ms, pq_.limit_ms);            // sub_5FA9F0
    }
    if (pq_.left_ms > 0) return;
    // timed out: S2C 1196 [u32 qid] (no case in the KR client — not sent), the master stops the party's run
    const int qid = pq_.qid;
    PartyServer::PqView v;
    if (server_.party().pq_view(char_uid_, v) && v.master == char_uid_) server_.party().pq_cancel(char_uid_, qid);
    pq_ = PartyQuestState{};
    LOG_INFO("GameClientSession %u: party quest %d timed out", session_id(), qid);
}

// sub_7295F0: a monster we own died — our active entry's monster tells the party (SYSCMD 5557).
void GameClientSession::pq_on_kill(uint32_t mob)
{
    if (!pq_.active() || pq_.monster != static_cast<uint16_t>(mob)) return;
    server_.party().pq_kill(char_uid_, static_cast<uint16_t>(mob));
}

// ── the NPC functions ────────────────────────────────────────────────────────────────────────────────

// The CPartyManager checks of the master (sub_86A1B0 IsInSameZone, sub_86A2E0 / sub_86A420 the lowest / highest
// level, sub_869A20 IsPlayingQuestMember): the members found in our zone.
GameClientSession::PqParty GameClientSession::pq_party()
{
    PqParty r;
    PartyServer::PqView v;
    if (!server_.party().pq_view(char_uid_, v)) return r;
    r.party  = v.party;
    r.master = v.master;
    r.count  = static_cast<int>(v.members.size());
    r.same_zone = true;
    r.min_level = 400;
    r.max_level = 0;
    for (const auto& [uid, sid] : v.members) {
        PqMember m;
        bool found = false;
        if (uid == char_uid_) { m = pq_member(); found = true; }
        else if (auto s = server_.session_of_char(uid)) { m = s->pq_member(); found = m.zone == zone_id_; }
        if (!found) { r.same_zone = false; r.all_found = false; continue; }
        if (m.level < r.min_level) r.min_level = m.level;
        if (m.level > r.max_level) r.max_level = m.level;
        if (m.busy) r.busy.push_back(m.name);
        r.uids.push_back(uid);
    }
    return r;
}

// NpcScript 153..157 CheckPartyQuest (sub_7E1B20).
int GameClientSession::pq_check(NpcEntity& n, int qid)
{
    (void)n;
    const MonsterQuestDef* d = server_.quest_db().party(qid);
    if (!d) { LOG_ERROR("CheckPartyQuest - not found QuestInfo, QuestId:%d", qid); return -1; }
    if (pq_.qid == qid && pq_.done) return 2;               // sub_7DBA70 (sub_7DBAA0's branch never answers 4)
    const PqParty p = pq_party();
    if (!p.party) return 6;                                 // sub_60D250
    if (p.master != char_uid_) return 7;                    // sub_7EB810
    if (!p.same_zone) return 8;                             // sub_86A1B0
    if (d->min_level > p.min_level || d->max_level < p.max_level) return 9;
    if (!p.busy.empty() || !p.all_found) {                  // sub_869A20 -> S2C 876
        PacketBuffer o(static_cast<Opcode>(876), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(p.busy.size()));
        for (const std::string& nm : p.busy) {
            const std::string t = to_client_text(nm);
            o.write_bytes(t.c_str(), t.size() + 1);
        }
        send(o);
        return 5;
    }
    return 1;
}

// NpcScript 158..162 AcceptPartyQuest (sub_7E1CC0) -> hAcceptPartyQuest::Send (sub_7EB870) -> the Master
// (OnQCmdAcceptPartyQuest: USPM_Get_PartyQuestUid, then the members' times done) -> OnOk (sub_79B920).
int GameClientSession::pq_accept(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt)
{
    const MonsterQuestDef* d = server_.quest_db().party(qid);
    if (!d) { LOG_ERROR("AcceptPartyQuest - not found QuestInfo, QuestId:%d", qid); return -1; }
    PqParty p = pq_party();
    if (p.master != char_uid_) return 4;                    // sub_7EB810 (no party: no master either)
    if (!p.party) return 3;                                 // sub_869A00
    if (!p.same_zone) return 5;
    if (d->min_level > p.min_level || d->max_level < p.max_level) return 6;
    if (!p.busy.empty() || !p.all_found) return 2;
    int msg = -1;
    if (!pq_arg(args, size, cnt, 1, msg)) return -1;        // GetResIdx
    if (d->grades.empty()) return -1;
    const uint16_t monster = static_cast<uint16_t>(d->monster_of(0));                     // sub_40FAA0(info, 0)
    const uint16_t target  = static_cast<uint16_t>(p.count * d->grades[0].cnt);
    const int32_t  limit   = 60000 * d->grades[0].time;
    const uint8_t  members = static_cast<uint8_t>(p.count);
    // the Master: USPM_Get_PartyQuestUid (the PartyQuest row) and USPM_Get_PartyQuestComCount (type 7)
    const uint64_t uid = server_.db().insert("INSERT INTO party_quest (quest_id, monster_id, target_count, record_time, "
                                             "member_count, reg_date) VALUES (%d, %d, %d, 0, 0, NOW())", qid, monster,
                                             target);
    if (!uid) {
        LOG_ERROR("cMasterServer::OnQCmdAcceptPartyQuest - failure accept party quest, CHRUID:%u", char_uid_);
        send_npc_cancel();                                  // 5101 -> hAcceptPartyQuest::OnCancel
        return -2;
    }
    std::vector<std::pair<uint32_t, uint8_t>> coms;
    {
        std::string list;
        for (uint32_t u : p.uids) list += (list.empty() ? "" : ",") + std::to_string(u);
        if (!list.empty())
            for (const DBRow& r : server_.db().query("SELECT char_uid, com_count FROM char_quest_history WHERE "
                                                    "quest_type=%d AND quest_id=%d AND char_uid IN (%s)",
                                                    kSqlParty, qid, list.c_str())) {
                auto a = r.find("char_uid"), b = r.find("com_count");
                if (a != r.end() && b != r.end())
                    coms.emplace_back(static_cast<uint32_t>(std::strtoul(a->second.c_str(), nullptr, 10)),
                                      static_cast<uint8_t>(std::atoi(b->second.c_str())));
            }
    }
    // OnOk: the zone again, every member's entry, the party's run, the NPC's message
    p = pq_party();
    if (!p.same_zone || !p.all_found) {
        LOG_ERROR("hAcceptPartyQuest::OnOk - partyMember do not exist in same area., CHRUID:%u, QuestId:%d", char_uid_, qid);
        send_npc_cancel();                                  // sub_79BEF0 (+ sub_79BF30: nothing was added)
        return -2;
    }
    const int64_t start = pq_now();
    PartyServer::PqView v;
    server_.party().pq_view(char_uid_, v);
    for (const auto& [mu, sid] : v.members) {
        uint8_t com = 0;
        for (const auto& c : coms) if (c.first == mu) com = c.second;
        if (mu == char_uid_) { pq_add(uid, qid, members, monster, target, start, limit, com); continue; }
        if (!sid) continue;
        server_.post_to_session(sid, [uid, qid, members, monster, target, start, limit, com](GameClientSession& s) {
            s.pq_add(uid, qid, members, monster, target, start, limit, com);
        });
    }
    if (!server_.party().pq_begin(char_uid_, uid, qid, monster, target, start, limit, coms))
        LOG_ERROR("PartyServer::BeginPartyQuest - failure begin partyQuest, CHRUID:%u, PTYID:%u", char_uid_, v.party);
    send_npc_message(n, msg, qid);                          // sub_5F91F0
    return -2;
}

// NpcScript 163..167 CompletePartyQuest (sub_7E1F00) -> hRewardPartyQuest::Send (sub_7EB9E0) -> the Master
// (OnQCmdCompletePartyQuest: USPM_Reward_PartyQuest + the items) -> OnOk (sub_79C470).
int GameClientSession::pq_complete(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt)
{
    int msg = -1;
    if (!pq_arg(args, size, cnt, 1, msg)) {
        LOG_ERROR("NpcScript::CompletePartyQuest - Failed GetResIdx, CHRUID:%u, questId:%d, cntArg:%d", char_uid_, qid, cnt);
        return -1;
    }
    if (pq_.qid != qid) {
        LOG_ERROR("hRewardPartyQuest::Send - not found questItem, QuestId:%d, CHRUID:%u", qid, char_uid_);
        return -1;
    }
    if (!pq_.done) {
        LOG_ERROR("hRewardPartyQuest::Send - don't sucess partyQuest, QuestId:%d, CHRUID:%u", qid, char_uid_);
        return -1;
    }
    const MonsterQuestDef* d = server_.quest_db().party(qid);
    if (!d) { LOG_ERROR("hRewardPartyQuest::Send - not found questInfo, QuestId:%d, CHRUID:%u", qid, char_uid_); return -1; }
    if (d->results.empty()) {
        LOG_ERROR("hRewardPartyQuest::Send - invalid questResult, QuestId:%d, CHRUID:%u", qid, char_uid_);
        return -1;
    }
    if (d->result_count <= 0) {
        LOG_ERROR("hRewardPartyQuest::Send - invalid questResultCount, QuestId:%d, CHRUID:%u", qid, char_uid_);
        return -1;
    }
    const MonsterQuestResult* res = d->result_of(0);        // sub_60C9D0(info, 0)
    if (!res) return -1;
    std::vector<QuestItem> items;
    if (res->type) {                                        // one of them at random (rand() % count)
        if (!res->items.empty()) items.push_back(res->items[static_cast<size_t>(std::rand() % static_cast<int>(res->items.size()))]);
    } else {
        items = res->items;
    }
    int weight = 0;                                         // sub_6EBA60
    for (const QuestItem& it : items)
        if (const ItemDef* idf = server_.item_db().get(it.item)) weight += idf->weight * idf->item_count(it.cnt);
    if (weight + bag_weight_ > st_maxwt_) return 2;
    if (300 - static_cast<int>(bag_.size()) < static_cast<int>(items.size())) return 2;   // sub_6E4290
    // the Master: USPM_Reward_PartyQuest
    int com = 0;
    const int err = db_pq_reward(qid, pq_.uid, com);
    if (err) {
        LOG_ERROR("cMasterServer::OnQCmdCompletePartyQuest - failure CompletedPartyQuest, CHRUID: %u, QUESTID:%d (%d)",
                  char_uid_, qid, err);
        send_npc_cancel();                                  // 5098 -> OnCancel
        return -2;
    }
    const std::vector<BagRecord> applied = quest_bag_records({}, items);
    pet_speech(15);                                         // our pet's line
    // sub_79CC60 / sub_79CDA0
    const double ratio = server_.quest_db().party_exp_ratio(com);
    const double share = pq_.target ? static_cast<double>(pq_.kills) / static_cast<double>(pq_.target) : 0.0;
    auto calc = [&](int base) {
        const double b = static_cast<double>(base);
        const double m = static_cast<double>(pq_.members);
        return static_cast<int32_t>(((m * 0.05 + 0.2) * b + (m - 1.0) * b * share) * ratio);
    };
    const int32_t exp  = d->grades.empty() ? 0 : calc(d->grades[0].exp);
    const int32_t tmxp = d->grades.empty() ? 0 : calc(d->grades[0].tmxp);
    {
        PacketBuffer p(static_cast<Opcode>(873), is_paula());   // sub_5FAC00
        p.write<uint32_t>(static_cast<uint32_t>(exp));
        p.write<uint32_t>(static_cast<uint32_t>(tmxp));
        p.write<uint16_t>(static_cast<uint16_t>(applied.size()));
        for (const BagRecord& r : applied) write_bag_record(p, r);
        p.write<uint32_t>(static_cast<uint32_t>(qid));
        send(p);
    }
    // Player::UpdatePartyQuestExp (sub_79CEE0)
    LOG_INFO("Player::UpdatePartyQuestExp - SUCCESS!! playerName:%s, CHRUID:%u, Count:%d, totalExp:%d, totalTmxp:%d",
             char_name_, char_uid_, com, exp, tmxp);
    if (exp && !add_exp(exp)) {
        PacketBuffer p(static_cast<Opcode>(0x0026), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(exp_));
        send(p);
    }
    if (tmxp && !add_tm_exp(tmxp)) {
        PacketBuffer p(static_cast<Opcode>(0x011D), is_paula());
        p.write<uint8_t>(0);
        p.write<uint32_t>(static_cast<uint32_t>(tm_exp_));
        send(p);
    }
    pq_ = PartyQuestState{};                                // sub_7DBA30
    send_npc_message(n, msg, qid);                          // sub_5F91F0
    return -2;
}

// ── the DB (the Paula procedures) ────────────────────────────────────────────────────────────────────

// hInsertPartyQuest (sub_79DEF0) -> USPM_Complete_PartyQuest(char, party uid, qid, our kills, the time left,
// members): the PartyQuest row's record; the times done = our char_quest_history row of the quest (monster 0,
// any type) + 1, at most 99, else 1; our PartyQuestMember row; char_quest type 7 (the received-quests tab
// until the reward).
void GameClientSession::pq_insert()
{
    server_.db().execute("UPDATE party_quest SET record_time=%d, member_count=%u WHERE party_uid=%llu", pq_.record,
                         static_cast<unsigned>(pq_.members), static_cast<unsigned long long>(pq_.uid));
    int com = 1;
    const DBResult h = server_.db().query("SELECT com_count FROM char_quest_history WHERE char_uid=%u AND quest_id=%d "
                                          "AND monster_id=0", char_uid_, pq_.qid);
    if (!h.empty()) {
        auto it = h[0].find("com_count");
        const int c = (it != h[0].end() && !it->second.empty()) ? std::atoi(it->second.c_str()) : 0;
        com = (c == 99) ? 99 : c + 1;
    }
    int job = 0;
    const DBResult cj = server_.db().query("SELECT job, job_2, job_3 FROM characters WHERE uid=%u", char_uid_);
    if (!cj.empty()) {
        auto g = [&](const char* k) { auto it = cj[0].find(k); return it == cj[0].end() ? 0 : std::atoi(it->second.c_str()); };
        job = g("job_2") == 0 ? g("job") : (g("job_3") == 0 ? g("job_2") : g("job_3"));
    }
    const uint64_t rows = server_.db().execute("INSERT INTO party_quest_member (party_uid, char_uid, level, char_job, "
                                               "kill_count, com_count, reward_date) VALUES (%llu, %u, %d, %d, %u, %d, NULL)",
                                               static_cast<unsigned long long>(pq_.uid), char_uid_, level_, job,
                                               static_cast<unsigned>(pq_.kills), com);
    if (rows != 1) {
        LOG_ERROR("cMasterServer::OnQCmdInsertPartyQuest - failure insert party quest, CHRUID:%u, PQUID:%llu, QUESTID:%d",
                  char_uid_, static_cast<unsigned long long>(pq_.uid), pq_.qid);
        return;
    }
    server_.db().execute("INSERT INTO char_quest (char_uid, quest_type, quest_id, com_count, monster_id, kill_count, "
                         "record_time) VALUES (%u, %d, %d, %d, 0, 0, 0)", char_uid_, kSqlParty, pq_.qid, com);
}

// USPM_Reward_PartyQuest(char, qid, party uid): the member row not rewarded yet gives the times done -> the
// history (1: a new row, more: its count and date), char_quest's row goes, the history log, the row rewarded.
// (tbl_guild_member: no guilds here.) 0 or the procedure's error.
int GameClientSession::db_pq_reward(int qid, uint64_t uid, int& com)
{
    const DBResult m = server_.db().query("SELECT q.monster_id, q.record_time, m.com_count, m.kill_count FROM party_quest q "
                                          "JOIN party_quest_member m ON q.party_uid=m.party_uid WHERE q.party_uid=%llu "
                                          "AND m.char_uid=%u AND m.reward_date IS NULL",
                                          static_cast<unsigned long long>(uid), char_uid_);
    if (m.empty()) return 60003;                            // 'Not Found PartyQuestMember or Aleady Reward'
    auto g = [&](const char* k) { auto it = m[0].find(k); return it == m[0].end() ? 0 : std::atoi(it->second.c_str()); };
    com = g("com_count");
    const int monster = g("monster_id"), kills = g("kill_count"), record = g("record_time");
    if (com == 1) {
        server_.db().execute("INSERT INTO char_quest_history (char_uid, quest_type, quest_id, com_count, monster_id, "
                             "kill_count, record_time, quest_completed_date) VALUES (%u, %d, %d, %d, 0, 0, 0, NOW())",
                             char_uid_, kSqlParty, qid, com);
    } else if (com > 1) {
        const uint64_t n = server_.db().execute("UPDATE char_quest_history SET com_count=%d, quest_completed_date=NOW() "
                                                "WHERE char_uid=%u AND quest_id=%d AND monster_id=0 AND quest_type=%d",
                                                com, char_uid_, qid, kSqlParty);
        if (n == 0) return 60002;
    } else {
        return 60003;
    }
    if (server_.db().execute("DELETE FROM char_quest WHERE char_uid=%u AND quest_id=%d AND monster_id=0", char_uid_, qid) == 0)
        return 60004;
    server_.db().execute("INSERT INTO char_quest_history_log (char_uid, quest_type, quest_id, com_count, monster_id, "
                         "kill_count, record_time, quest_completed_date) VALUES (%u, %d, %d, %d, %d, %d, %d, NOW())",
                         char_uid_, kSqlParty, qid, com, monster, kills, record);
    if (server_.db().execute("UPDATE party_quest_member SET reward_date=NOW() WHERE party_uid=%llu AND char_uid=%u AND "
                             "reward_date IS NULL", static_cast<unsigned long long>(uid), char_uid_) == 0)
        return 60006;
    return 0;
}

// The login (OnQCmdSelectPartyQuestAckOk sub_40E9F0 -> sub_7DB730): USPM_GetList_PartyQuest — the party quests
// done whose reward waits; each row fills the one entry as done (the last row stays).
void GameClientSession::pq_load()
{
    pq_ = PartyQuestState{};
    const DBResult rows = server_.db().query("SELECT q.party_uid, q.quest_id, q.monster_id, m.kill_count, q.target_count, "
                                             "q.record_time, q.member_count, m.com_count FROM party_quest q JOIN "
                                             "party_quest_member m ON q.party_uid=m.party_uid WHERE m.char_uid=%u AND "
                                             "m.reward_date IS NULL", char_uid_);
    for (const DBRow& r : rows) {
        auto g = [&](const char* k) { auto it = r.find(k); return it == r.end() ? 0LL : std::atoll(it->second.c_str()); };
        PartyQuestState e;
        e.uid     = static_cast<uint64_t>(g("party_uid"));
        e.qid     = static_cast<int>(g("quest_id"));
        e.monster = static_cast<uint16_t>(g("monster_id"));
        e.kills   = static_cast<uint16_t>(g("kill_count"));
        e.target  = static_cast<uint16_t>(g("target_count"));
        e.record  = static_cast<int32_t>(g("record_time"));
        e.members = static_cast<uint8_t>(g("member_count"));
        e.com     = static_cast<uint16_t>(g("com_count"));
        e.done    = true;
        pq_ = e;
    }
    if (pq_.qid) LOG_INFO("GameClientSession %u: party quest %d waits for its reward", session_id(), pq_.qid);
}

// ── the zone-change carry ────────────────────────────────────────────────────────────────────────────

void cGameServer::pq_put(uint32_t char_uid, const PartyQuestState& s)
{
    std::lock_guard<std::mutex> lock(pq_carry_mutex_);
    pq_carry_[char_uid] = { s, std::chrono::steady_clock::now() };
}

bool cGameServer::pq_take(uint32_t char_uid, PartyQuestState& out)
{
    std::lock_guard<std::mutex> lock(pq_carry_mutex_);
    auto it = pq_carry_.find(char_uid);
    if (it == pq_carry_.end()) return false;
    const auto e = it->second;
    pq_carry_.erase(it);
    if (std::chrono::steady_clock::now() - e.second > std::chrono::seconds(60)) return false;
    out = e.first;
    return true;
}

void GameClientSession::pq_carry_out()
{
    if (pq_.qid) server_.pq_put(char_uid_, pq_);
}

void GameClientSession::pq_restore_carry()
{
    PartyQuestState s;
    if (server_.pq_take(char_uid_, s)) pq_ = s;
}

// ── the SystemServer's side (PartyServer) ────────────────────────────────────────────────────────────

bool PartyServer::pq_view(uint32_t uid, PqView& out)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    out = PqView{};
    Client* c = client(uid);
    Party* pt = c ? party_of(*c) : nullptr;
    if (!pt) return false;
    out.party = pt->id;
    if (Client* m = client_id(pt->master)) out.master = m->uid;
    for (uint16_t id : pt->members)
        if (Client* m = client_id(id)) out.members.emplace_back(m->uid, m->moving ? 0u : m->sid);
    return true;
}

bool PartyServer::pq_begin(uint32_t uid, uint64_t pq_uid, int qid, uint16_t monster, uint16_t target, int64_t start,
                           int32_t limit, const std::vector<std::pair<uint32_t, uint8_t>>& coms)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    Party* pt = c ? party_of(*c) : nullptr;
    if (!pt) {
        LOG_ERROR("PartyServer::BeginPartyQuest - not found party, CHRUID:%u", uid);
        return false;
    }
    if (pt->pq.qid) {                                       // CPartyQuestMonster::Initialize
        LOG_ERROR("CPartyQuestMonster::Initialize - already play partyQuest, questId:%d", qid);
        return false;
    }
    Party::PqRun r;
    r.qid = qid; r.uid = pq_uid; r.monster = monster; r.target = target; r.start = start; r.limit = limit;
    for (uint16_t id : pt->members)
        if (Client* m = client_id(id)) {
            Party::PqKill k;
            k.uid = m->uid;
            for (const auto& cc : coms) if (cc.first == m->uid) k.com = cc.second;
            r.recs.push_back(k);
        }
    pt->pq = r;
    return true;
}

// Party::PartyQuestKillMonster (sub_4F4F40): playing, its monster, not done, a member with a record -> one more,
// every member's update; the target reached -> the run ends.
void PartyServer::pq_kill(uint32_t uid, uint16_t monster)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) { LOG_ERROR("PartyServer::OnPartyQuestKillMonster - not partyMember, CHRUID:%u", uid); return; }
    Party* pt = party_of(*c);
    if (!pt) { LOG_ERROR("PartyServer::OnPartyQuestKillMonster - invalid party, CHRUID:%u", uid); return; }
    Party::PqRun& r = pt->pq;
    if (!r.qid) return;                                     // "do not playing partyQuest"
    if (r.monster != monster) {
        LOG_ERROR("CPartyQuestMonster::UpdateKillMonster - not partyQuest Monster, DEST MonsterId:%u, SRC MonsterId:%u",
                  r.monster, monster);
        return;
    }
    if (r.total == r.target) {
        LOG_ERROR("CPartyQuestMonster::UpdateKillMonster - already completed partyQuest, QUESTID:%d", r.qid);
        return;
    }
    Party::PqKill* k = nullptr;
    for (Party::PqKill& x : r.recs) if (x.uid == uid) k = &x;
    if (!k) { LOG_ERROR("CPartyQuestMonster::UpdateKillMonster - not found player, CHRUID:%u", uid); return; }
    ++k->kills;
    ++r.total;
    pq_notify(*pt);
    if (r.total == r.target) {
        LOG_INFO("Party::PartyQuestKillMonster - success partyQuest, PQUID:%llu, masterChrUid:%u",
                 static_cast<unsigned long long>(r.uid), uid);
        r = Party::PqRun{};                                 // sub_511F20
    }
}

void PartyServer::pq_cancel(uint32_t uid, int qid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    Party* pt = c ? party_of(*c) : nullptr;
    if (!pt) return;
    if (!pt->pq.qid) {
        LOG_ERROR("Party::CancelPartyQuest - do not playing partyQuest, PTYID:%u, CHRUID:%u, QuestId:%d", pt->id, uid, qid);
        return;
    }
    pt->pq = Party::PqRun{};
}

void PartyServer::pq_remove(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    Party* pt = c ? party_of(*c) : nullptr;
    if (!pt || pt->master != c->id) { LOG_ERROR("PartyMember::OnCmdRemovePartyQuest - not partyMaster"); return; }
    if (!pt->pq.qid) {
        LOG_INFO("PartyMember::OnCmdRemovePartyQuest - do not play partyQuest, CHRUID:%u", uid);
        return;
    }
    pq_notify_cancel(*pt, 3);
    pt->pq = Party::PqRun{};
}

void PartyServer::pq_notify(Party& pt)
{
    const int qid = pt.pq.qid;
    const uint16_t total = pt.pq.total;
    std::vector<PqRec> recs;
    for (const Party::PqKill& k : pt.pq.recs) {
        if (recs.size() >= 6) break;
        recs.push_back({ k.uid, k.kills });
    }
    for (uint16_t id : pt.members)
        if (Client* m = client_id(id))
            if (m->sid && !m->moving)
                gs_.post_to_session(m->sid, [qid, total, recs](GameClientSession& s) { s.pq_update(qid, total, recs); });
}

void PartyServer::pq_notify_cancel(Party& pt, uint32_t why)
{
    const int qid = pt.pq.qid;
    for (uint16_t id : pt.members)
        if (Client* m = client_id(id))
            if (m->sid && !m->moving)
                gs_.post_to_session(m->sid, [qid, why](GameClientSession& s) { s.pq_cancelled(qid, why); });
}

void PartyServer::pq_entered(Party& pt, Client& c)
{
    if (!c.sid) return;
    const Party::PqRun r = pt.pq;
    uint16_t com = 0;
    for (const Party::PqKill& k : r.recs) if (k.uid == c.uid) com = k.com;
    const uint8_t members = static_cast<uint8_t>(pt.members.size());
    gs_.post_to_session(c.sid, [r, members, com](GameClientSession& s) {
        s.pq_party_state(r.qid, r.uid, members, r.monster, r.target, r.start, r.limit, com);
    });
    if (r.qid) pq_notify(pt);                               // OnNotifyEnterZone (sub_4F1DE0)
}
