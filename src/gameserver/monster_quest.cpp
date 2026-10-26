#include "game_server.hpp"
#include "game_session.hpp"
#include "logger.hpp"
#include "client_text.hpp"
#include <algorithm>
#include <cstdlib>

// ─────────────────────────────────────────────────────────────────────────────
//  Monster quests — 1:1 with the retail GameServer (GameServerRelease) and its MasterServer QuestStore:
//
//  A MonsterQuestInfo has up to five grades (SrcMonsterN..MaxLvN): the player takes the one whose level
//  range holds his level and must kill CntN of SrcMonsterN within TimeN minutes. One monster quest at a
//  time (MonsterQuestInv, Player +2228, one entry) and its running state (Player +3740).
//  The NPC script functions (ExecuteNpcScript sub_7E85A0; qid = the NPC's QuestId[slot]):
//    MonQuest_Check (7, _1.._4 = 238..241) sub_7E1720: another running monster quest (or a party quest)
//      -> 6; no entry and none free -> 6; in progress: run above MaxCount -> 4, not killed yet -> 3, done
//      (checked against the running state) -> 2; not taken: no grade for our level / level out of
//      MinLevel..MaxLevel -> 5, else uspm_quest_check_history (by the grade's monster) -> S2C 115 with the
//      1st argument (or the 4th when done MaxCount times).
//    MonQuest_Accept (8, 242..245) sub_7E2010: -1 another running / taken already, 3 no grade, 2 no free
//      entry; uspm_quest_take (nothing written: the row comes when the kills are done) -> the entry with
//      the grade of our level, S2C 210, the timer S2C 215 [u32 left][u32 total], the kills S2C 217 [u16 0]
//      [u16 target], then S2C 115 with the 1st argument. MonQuest_Continue (10, 250..253) = Accept
//      answering -2 or -1; MonQuest_Quit (11) = 1; MonQuest_Once_Accept (185) adds the day check.
//    MonQuest_Complete (9, 246..249) / MonQuest_NH_Complete (149, no history) sub_7E2680: not killed
//      (bSuccess 0) or a bad count -> the player is dropped; the grade's result (Type 1: one item at
//      random), weight / free entries -> 2; the message: run under MaxCount -> 1 (our level still in the
//      grade) / 4 (another grade) / 5 (none), else 3; uspm_quest_completed + the items -> OnOk
//      (sub_77E2F0): money (S2C 41), the grade's EXP / TM EXP, S2C 219, S2C 115, S2C 211, the state off.
//    CheckMonQuestState (83) sub_7E5520: three arguments; CheckMonQuestNum > 0 -> history count (type 2)
//      -> the 2nd / 3rd argument, else the quest killed -> 1 (1st) / 3 (3rd).
//  The kills (CommonMonster::ProcessMonsterQuest sub_729410, from the die handler, for the monster's
//  owner): a running quest whose grade monster it is -> +1, S2C 217; the target reached -> bSuccess,
//  the record (the time taken), S2C 218 [u16 qid][u8 ReturnMapID set] ("Alert_MonQuestSuccess"; with a
//  return map the client offers the portal), uspm_quest_monster_allkill (the char_quest row).
//  The timer (Player::ProcessMonsterQuest sub_75E780, every tick): S2C 215 every second; at 0 -> S2C 216
//  ("Alert_MonQuestTimeOut"), the state off; killed -> uspm_quest_monster_update, else S2C 211 and the
//  entry goes. Dying does the same (Player::OnObjMsg_Die). The return portal: C2S 1040 [u32 2] -> a
//  running quest with a ReturnMapID 1..1000 -> the field change (type 4) in 500 ms + S2C 0x411, else
//  S2C 0x412 (client "PQ_MsgMonQuestReturnMapOk / Cancel").
//  Wire VERIFIED in the client (sub_6F0960): 210 [u16 qid][u16 monster][u8 grade][u8 run][u16 kills]
//  [u32 record][cstr Name][cstr Client][cstr Pos][cstr Comment]; 211 [u16 qid]; 215; 216 [u16 qid]; 217;
//  218; 219 [u16 n]{record}[u16 m]{u32 item, u16 count}[u32 EXP][u32 TM EXP][u16 qid][u32 gelder];
//  235 (login) [u16 n]{u16 qid, u16 monster, u8 run, u8 grade, u16 kills, u32 record}.
//  Not modelled: the rankings (MonQuest_ZoneRank / AllRank: S2C 7510 / 7509 go to another client
//  dispatcher; the record update 5529 to the SystemServer), the tutorial monster quests (type 5), the
//  pet's messages, the death copy kept for a beginner's restart (+4780), achievements.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// sub_7EB070 (see quest.cpp): the idx-th u16 argument, read from the start.
bool monq_arg(const uint8_t* p, size_t size, int cnt, int idx, int& out)
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

constexpr int kSqlMonster = 2;

}  // namespace

// ── The zone-change carry ─────────────────────────────────────────────────────────────────────────

void cGameServer::monq_put(uint32_t char_uid, const MonqCarry& c)
{
    std::lock_guard<std::mutex> lock(monq_carry_mutex_);
    MonqCarry e = c;
    e.at = std::chrono::steady_clock::now();
    monq_carry_[char_uid] = e;
}

bool cGameServer::monq_take(uint32_t char_uid, MonqCarry& out)
{
    std::lock_guard<std::mutex> lock(monq_carry_mutex_);
    auto it = monq_carry_.find(char_uid);
    if (it == monq_carry_.end()) return false;
    const MonqCarry e = it->second;
    monq_carry_.erase(it);
    if (std::chrono::steady_clock::now() - e.at > std::chrono::seconds(60)) return false;
    out = e;
    return true;
}

// ── The inventory ─────────────────────────────────────────────────────────────────────────────────

GameClientSession::MonQuestEntry* GameClientSession::monq_find(int qid)
{
    return (monq_.used && monq_.qid == qid) ? &monq_ : nullptr;
}

// sub_7DA620: the entry of qid (sub_7DA560: the free one, run 1) gets the run `count` and the grade of
// `level`; no grade -> the entry goes again.
GameClientSession::MonQuestEntry* GameClientSession::monq_add(int qid, int count, int level)
{
    MonQuestEntry* e = monq_find(qid);
    if (!e) {
        if (monq_.used) return nullptr;
        monq_ = MonQuestEntry{};
        monq_.used  = true;
        monq_.qid   = qid;
        monq_.def   = server_.quest_db().monster(qid);
        monq_.count = 1;
        e = &monq_;
    }
    e->count = count;
    const int g = e->def ? e->def->grade_for(level) : -1;
    if (g < 0) {
        monq_remove(qid);
        return nullptr;
    }
    e->grade   = g;
    e->comment = e->def->comment;
    return e;
}

bool GameClientSession::monq_remove(int qid)
{
    if (!monq_find(qid)) return false;
    monq_ = MonQuestEntry{};                             // sub_79E600
    return true;
}

// The login: char_quest types 2 / 5 (a monster quest is written when its kills are done) — {quest_id,
// monster_id, com_count, kill_count, record_time}, the MasterServer adds bSuccess 1 (sub_46DC30); at most
// one ("create client : invalid monster quest item cnt"); the grade = the one of that monster (sub_7DA720).
void GameClientSession::load_monster_quest()
{
    monq_ = MonQuestEntry{};
    mqs_  = MonQuestState{};                             // sub_76CCD0
    if (!char_uid_) return;
    const DBResult rows = server_.db().query(
        "SELECT quest_id, monster_id, com_count, kill_count, record_time FROM char_quest WHERE char_uid=%u AND "
        "(quest_type=2 OR quest_type=5)", char_uid_);
    if (rows.size() > 1)
        LOG_ERROR("%s : create client : invalid monster quest item cnt %zu", char_name_, rows.size());
    for (const DBRow& r : rows) {
        const int qid = std::atoi(r.at("quest_id").c_str());
        const int mon = std::atoi(r.at("monster_id").c_str());
        const MonsterQuestDef* d = server_.quest_db().monster(qid);
        const int g = d ? d->grade_of_monster(mon) : -1;
        if (g < 0) {
            LOG_ERROR("<%s> NetworkMsgHandler::ReserveChrLoginInfo : inv: MonQuest is invalid : qid:%d, monid:%d)",
                      char_name_, qid, mon);
            continue;
        }
        monq_.used    = true;
        monq_.qid     = qid;
        monq_.def     = d;
        monq_.count   = std::atoi(r.at("com_count").c_str());
        monq_.success = true;
        monq_.kills   = std::atoi(r.at("kill_count").c_str());
        monq_.record  = std::atoi(r.at("record_time").c_str());
        monq_.comment = d->comment;
        monq_.grade   = g;
        break;
    }
}

// A zone change: the leaving zone's MonsterQuestInv and state block (Client::PrepareLeavingZone) replace
// what the DB has.
void GameClientSession::monq_restore_carry()
{
    cGameServer::MonqCarry c;
    if (!server_.monq_take(char_uid_, c)) return;
    monq_ = MonQuestEntry{};
    if (c.used) {
        monq_.used    = true;
        monq_.done    = c.done;
        monq_.count   = c.count;
        monq_.qid     = c.qid;
        monq_.def     = server_.quest_db().monster(c.qid);
        monq_.grade   = c.grade;
        monq_.comment = monq_.def ? monq_.def->comment : std::string();
        monq_.success = c.success;
        monq_.kills   = c.kills;
        monq_.record  = c.record;
    }
    mqs_.flag      = c.flag;
    mqs_.active    = c.active;
    mqs_.remain_ms = c.remain_ms;
    mqs_.acc_ms    = c.acc_ms;
    mqs_.kills     = c.st_kills;
    mqs_.target    = c.target;
    mqs_.qid       = c.st_qid;
    mqs_.grade     = c.st_grade;
}

void GameClientSession::monq_carry_out()
{
    // sub_75E9D0: a killed quest still running keeps its char_quest kills up to date (the map change).
    if (const MonQuestEntry* e = monq_find(mqs_.qid))
        if (e->success && mqs_.active)
            db_quest_monster_update(e->qid, e->def ? e->def->monster_of(e->grade) : 0, mqs_.kills);
    cGameServer::MonqCarry c;
    c.used      = monq_.used;
    c.done      = monq_.done;
    c.success   = monq_.success;
    c.count     = monq_.count;
    c.qid       = monq_.qid;
    c.grade     = monq_.grade;
    c.kills     = monq_.kills;
    c.record    = monq_.record;
    c.flag      = mqs_.flag;
    c.active    = mqs_.active;
    c.remain_ms = mqs_.remain_ms;
    c.acc_ms    = mqs_.acc_ms;
    c.st_kills  = mqs_.kills;
    c.target    = mqs_.target;
    c.st_qid    = mqs_.qid;
    c.st_grade  = mqs_.grade;
    server_.monq_put(char_uid_, c);
}

// S2C 235 (sub_5F6D60, the login sequence).
void GameClientSession::send_monster_quest_list()
{
    PacketBuffer p(static_cast<Opcode>(0x00EB), is_paula());
    p.write<uint16_t>(monq_.used ? 1 : 0);
    if (monq_.used) {
        p.write<uint16_t>(static_cast<uint16_t>(monq_.qid));
        p.write<uint16_t>(static_cast<uint16_t>(monq_.def ? monq_.def->monster_of(monq_.grade) : 0));
        p.write<uint8_t>(static_cast<uint8_t>(monq_.count));
        p.write<uint8_t>(static_cast<uint8_t>(monq_.grade));
        p.write<uint16_t>(static_cast<uint16_t>(monq_.kills));
        p.write<uint32_t>(static_cast<uint32_t>(monq_.record));
    }
    send(p);
}

// S2C 210 (sub_5FA510).
void GameClientSession::send_monster_quest_add(const MonQuestEntry& e)
{
    if (!e.def) return;
    PacketBuffer p(static_cast<Opcode>(0x00D2), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(e.def->id));
    p.write<uint16_t>(static_cast<uint16_t>(e.def->monster_of(e.grade)));
    p.write<uint8_t>(static_cast<uint8_t>(e.grade));
    p.write<uint8_t>(static_cast<uint8_t>(e.count));
    p.write<uint16_t>(static_cast<uint16_t>(e.kills));
    p.write<uint32_t>(static_cast<uint32_t>(e.record));
    for (const std::string* s : { &e.def->name, &e.def->client, &e.def->pos, &e.comment }) {
        const std::string t = to_client_text(*s);
        p.write_bytes(t.c_str(), t.size() + 1);
    }
    send(p);
}

// S2C 211 (sub_5FA6C0): [u16 qid].
void GameClientSession::send_monster_quest_remove(int qid)
{
    PacketBuffer p(static_cast<Opcode>(0x00D3), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(qid));
    send(p);
}

// ── The running quest ─────────────────────────────────────────────────────────────────────────────

// Player::ProcessMonsterQuest (sub_75E780): the time left, S2C 215 [u32 left][u32 total] every second;
// at 0: S2C 216 [u16 qid], the entry keeps the kills, the state stops; a killed quest keeps its char_quest
// row up to date (uspm_quest_monster_update), another one leaves (S2C 211).
void GameClientSession::monq_tick(int32_t elapsed_ms)
{
    if (!mqs_.active) return;
    mqs_.remain_ms -= elapsed_ms;
    mqs_.acc_ms    += elapsed_ms;
    MonQuestEntry* e = monq_find(mqs_.qid);
    if (mqs_.acc_ms >= 1000) {
        mqs_.acc_ms = 0;
        if (mqs_.remain_ms <= 10000) pet_speech(16);         // our pet's line: time is running out
        const int total =(e && e->def && e->grade >= 0 && e->grade < static_cast<int>(e->def->grades.size()))
            ? 60000 * e->def->grades[static_cast<size_t>(e->grade)].time : 0;
        PacketBuffer p(static_cast<Opcode>(0x00D7), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(mqs_.remain_ms));
        p.write<uint32_t>(static_cast<uint32_t>(total));
        send(p);
    }
    if (mqs_.remain_ms > 0) return;
    {
        PacketBuffer p(static_cast<Opcode>(0x00D8), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(e ? e->qid : 0));
        send(p);
    }
    if (e) e->kills = mqs_.kills;                        // sub_72C0E0
    mqs_.active    = false;
    mqs_.acc_ms    = 0;
    mqs_.remain_ms = 0;
    if (!e) {
        LOG_ERROR("%s : Player::ProcessMonsterQuest:m_MonQuestInv.Get(%d) not found", char_name_, mqs_.qid);
        return;
    }
    if (e->success) {
        db_quest_monster_update(e->qid, e->def ? e->def->monster_of(e->grade) : 0, mqs_.kills);
    } else {
        const int q = mqs_.qid;
        monq_remove(q);
        send_monster_quest_remove(q);
        LOG_INFO("GameClientSession %u: monster quest %d timed out (%d kills)", session_id(), q, mqs_.kills);
    }
}

// CommonMonster::ProcessMonsterQuest (sub_729410): a monster we own died.
void GameClientSession::monq_on_kill(uint32_t mob_id)
{
    MonQuestEntry* e = monq_find(mqs_.qid);
    if (!e || !mqs_.active || !e->def) return;
    if (e->def->monster_of(e->grade) != static_cast<int>(mob_id)) return;
    e->kills = ++mqs_.kills;                             // sub_72C0E0
    {
        PacketBuffer p(static_cast<Opcode>(0x00D9), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(mqs_.kills));
        p.write<uint16_t>(static_cast<uint16_t>(mqs_.target));
        send(p);
    }
    if (mqs_.kills != mqs_.target) return;
    const int mon = e->def->monster_of(e->grade);
    e->success = true;                                   // sub_72C0B0
    const int total = 60000 * e->def->grades[static_cast<size_t>(e->grade)].time;
    const int record = total - mqs_.remain_ms;
    e->record = record;                                  // sub_72C110
    LOG_DEBUG("CommonMonster::ProcessMonsterQuest:%s - qid:%d, monster:%u, grade:%d, time:%d, curtime:%d, record:%d, "
              "kill:%d", char_name_, mqs_.qid, mob_id, e->grade, e->def->grades[static_cast<size_t>(e->grade)].time,
              mqs_.remain_ms, record, mqs_.kills);
    {
        PacketBuffer p(static_cast<Opcode>(0x00DA), is_paula());   // S2C 218 (sub_5F4B10)
        p.write<uint16_t>(static_cast<uint16_t>(mqs_.qid));
        p.write<uint8_t>(e->def->return_map != 0 ? 1 : 0);
        send(p);
    }
    db_quest_monster_allkill(e->qid, mon, mqs_.kills, record);   // sub_5F19C0 -> 5079
}

// The die handler's owner part (sub_7296E0 -> sub_729410): the monster's owner, a player, counts the kill —
// on his own strand when it is someone else.
void GameClientSession::owner_monster_quest(const MonsterEntity& m)
{
    if (!m.owner_oid) return;
    const uint32_t mob = m.mob_id;
    // (then sub_7295F0: the owner's party quest, party_quest.cpp)
    if (m.owner_oid == object_id_) { monq_on_kill(mob); pq_on_kill(mob); return; }
    GameClientSession* o = server_.player_session(m.owner_oid);
    if (o) o->run_on_strand([o, mob]() { o->monq_on_kill(mob); o->pq_on_kill(mob); });
}

// Player::OnObjMsg_Die: a running monster quest stops — killed: the kills are saved; else it is lost.
void GameClientSession::monq_on_death()
{
    if (!mqs_.active) return;
    mqs_.active    = false;
    mqs_.acc_ms    = 0;
    mqs_.remain_ms = 0;
    MonQuestEntry* e = monq_find(mqs_.qid);
    if (!e) {
        LOG_ERROR("%s : Player::OnObjMsg_Die:m_MonQuestInv.Get(%d) not found", char_name_, mqs_.qid);
        return;
    }
    if (e->success) {
        db_quest_monster_update(e->qid, e->def ? e->def->monster_of(e->grade) : 0, mqs_.kills);
    } else {
        const int q = mqs_.qid;
        send_monster_quest_remove(q);
        monq_remove(q);
    }
}

// C2S 1040 [u32 type] (the client's return portal, sub_9C0E20): type 2 = the monster quest. A running quest
// with a ReturnMapID 1..1000 -> the field change of type 4 to that map in 500 ms (sub_50D810; the position
// (0, 0): the map's first type-9 point, else its arrival) and S2C 0x411; anything else -> S2C 0x412.
void GameClientSession::handle_monq_return(PacketBuffer& pkt)
{
    try {
        const uint32_t type = pkt.read<uint32_t>();
        const MonsterQuestDef* d = nullptr;
        bool active = false, flag = false;
        if (type == 2) {
            active = mqs_.active;
            flag   = mqs_.flag;
            d      = server_.quest_db().monster(mqs_.qid);
        } else if (type == 9) {
            // the party quest (@0x7498EA): both flags = sub_7DBA70, our entry done (party_quest.cpp)
            active = flag = (pq_.qid && pq_.done);
            d = server_.quest_db().party(pq_.qid);
        }
        const int map = d ? d->return_map : 0;
        if (!d || !active || !flag || map < 1 || map > 1000) {
            PacketBuffer c(static_cast<Opcode>(0x0412), is_paula());
            send(c);
            return;
        }
        LOG_INFO("GameClientSession %u: monster quest %d return portal -> map %d", session_id(), mqs_.qid, map);
        npc_move_after(map, 0, 0, 500);
        PacketBuffer ok(static_cast<Opcode>(0x0411), is_paula());
        send(ok);
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 1040 return portal aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 1040 return portal aborted: unknown exception", session_id());
    }
}

// ── The script functions ──────────────────────────────────────────────────────────────────────────

// sub_60CAF0: the NPC's MonsterQuestInfo (one per QuestId, Npc +1704) whose Id is qid.
const MonsterQuestDef* GameClientSession::npc_monster_quest_def(const NpcEntity& n, int qid) const
{
    if (!n.row) return nullptr;
    for (int i = 0; i < 5; ++i) {
        const MonsterQuestDef* d = server_.quest_db().monster(n.row->quest_id(i));
        if (d && static_cast<int16_t>(d->id) == static_cast<int16_t>(qid)) return d;
    }
    return nullptr;
}

int GameClientSession::monq_check(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt)
{
    if (mqs_.active && mqs_.qid != qid) return 6;
    // (sub_7EB720: a party quest in progress -> 6; no party quests here)
    const MonsterQuestDef* d = server_.quest_db().monster(qid);
    if (!d) return -1;
    MonQuestEntry* e = monq_find(qid);
    if (!mqs_.active && !e && monq_.used) return 6;      // no free entry
    if (e) {
        if (e->count > d->max_count) return 4;
        if (!e->success) return 3;
        const int need = (e->grade >= 0 && e->grade < static_cast<int>(d->grades.size()))
            ? d->grades[static_cast<size_t>(e->grade)].cnt : 0;
        if (mqs_.active && (mqs_.kills <= 0 || mqs_.kills < mqs_.target || mqs_.kills < need)) {
            LOG_ERROR("CheckMonQuest:%s - mqs.killCount:%d is illegal. mqs.targetcount:%d, killcountinfo:%d", char_name_,
                      mqs_.kills, mqs_.target, need);
            return -1;
        }
        if (e->kills > 0 && e->kills >= need) return 2;
        LOG_ERROR("CheckMonQuest:%s - qitem.killCount:%d is illegal. targetkillcountinfo:%d", char_name_, e->kills, need);
        return -1;
    }
    const int g = d->grade_for(level_);                  // sub_7EB790
    if (g < 0 || !d->level_ok(level_)) return 5;
    int yes = -1, no = -1;
    if (!monq_arg(args, size, cnt, 1, yes) || !monq_arg(args, size, cnt, 4, no)) {
        LOG_ERROR("%s Invalid CheckMonsterQuest res:%d, cntArg:%d, npctype:%d, zoneNo:%d", char_name_, yes < 1 ? 1 : 4,
                  cnt, n.row ? n.row->type : 0, zone_id_);
        return -1;
    }
    int com = 0;
    const int err = db_quest_check_history(kSqlMonster, qid, d->max_count, d->grades[static_cast<size_t>(g)].src_monster, com);
    if (err != 0 && err != 1001 && err != 1002) {
        send_npc_cancel();
        return -2;
    }
    send_npc_message(n, err == 0 ? yes : no, qid);
    return -2;
}

int GameClientSession::monq_accept(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt, bool once)
{
    if (mqs_.active && mqs_.qid != qid) return -1;
    const MonsterQuestDef* d = server_.quest_db().monster(qid);
    if (!d) return -1;
    if (monq_find(qid)) return -1;                       // sub_77C2A0
    const int g = d->grade_for(level_);
    if (g < 0 || !d->level_ok(level_)) {
        LOG_INFO("%s: %s: qid:%d: lev:%d: minlev:%d, maxlev:%d : NULL == pSrcMonster || monquest_IsValidLevel( qi, player "
                 ") == false", once ? "AcceptMonQuestOnce" : "AcceptMonQuest", char_name_, qid, level_, d->min_level,
                 d->max_level);
        return 3;
    }
    if (monq_.used) {
        LOG_INFO("%s: %s: qid:%d: NULL == player.m_MonQuestInv.GetEmpty()", once ? "AcceptMonQuestOnce" : "AcceptMonQuest",
                 char_name_, qid);
        return 2;
    }
    int msg = -1, today_msg = -1;
    if (!monq_arg(args, size, cnt, 1, msg) || (once && !monq_arg(args, size, cnt, 4, today_msg))) {
        LOG_ERROR("%s Invalid %s res:%d, cntArg:%d, npctype:%d, zoneNo:%d", char_name_,
                  once ? "AcceptMonQuestOnce" : "AcceptMonsterQuest", msg < 1 ? 1 : 4, cnt, n.row ? n.row->type : 0,
                  zone_id_);
        return -1;
    }
    const int mon = d->grades[static_cast<size_t>(g)].src_monster;
    if (once && db_quest_done_today(kSqlMonster, qid, mon)) {
        send_npc_message(n, today_msg, qid);
        return -2;
    }
    int com = 0;
    const int err = db_quest_take(kSqlMonster, qid, d->max_count, mon, com);
    if (err > 0) {
        LOG_ERROR("QuestStore::AcceptQuest SP failure : spRet:%d, chr:%u, qType:%d, qid:%d, maxCnt:%d, Count:%d, mid:%d",
                  err, char_uid_, kSqlMonster, qid, d->max_count, com, mon);
        send_npc_cancel();
        return -2;
    }
    // hItemAcceptQuest::OnOk type 2 (sub_77CAD0): the entry with our level's grade, S2C 210, the timer
    // (sub_75E290: S2C 215) and the kills (sub_75E320: S2C 217), the state's grade.
    MonQuestEntry* e = monq_add(qid, com, level_);
    if (!e) {
        db_quest_cancel(qid, mon);
        LOG_ERROR("-- hItemAcceptQuest::OnOk - but !bRet : name(%s), qid(%d), qtype(%d)", char_name_, qid, 2);
        send_npc_cancel();
        return -2;
    }
    send_monster_quest_add(*e);
    const MonsterQuestGrade& gr = d->grades[static_cast<size_t>(e->grade)];
    const int total = 60000 * gr.time;
    mqs_.active    = true;
    mqs_.remain_ms = total;
    mqs_.acc_ms    = 0;
    mqs_.qid       = qid;
    {
        PacketBuffer p(static_cast<Opcode>(0x00D7), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(total));
        p.write<uint32_t>(static_cast<uint32_t>(total));
        send(p);
    }
    mqs_.kills  = 0;
    mqs_.target = gr.cnt;
    {
        PacketBuffer p(static_cast<Opcode>(0x00D9), is_paula());
        p.write<uint16_t>(0);
        p.write<uint16_t>(static_cast<uint16_t>(gr.cnt));
        send(p);
    }
    mqs_.grade = e->grade;
    LOG_INFO("GameClientSession %u: monster quest %d taken (run %d, grade %d: %d x monster %d in %d min)", session_id(),
             qid, com, e->grade, gr.cnt, gr.src_monster, gr.time);
    if (msg < 512) send_npc_message(n, msg, qid);
    return -2;
}

int GameClientSession::monq_complete(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt, bool history)
{
    if (mqs_.active && mqs_.qid != qid) return -1;
    const MonsterQuestDef* d = server_.quest_db().monster(qid);
    if (!d) return -1;
    MonQuestEntry* e = monq_find(qid);
    if (!e) return -1;
    if (!e->success) {
        LOG_ERROR("CompleteMonQuest:%s - qItem.bSuccess = false", char_name_);
        close();                                         // sub_8AA290
        return -1;
    }
    if (e->grade < 0 || e->grade >= static_cast<int>(d->grades.size())) return -1;
    const MonsterQuestGrade& gr = d->grades[static_cast<size_t>(e->grade)];
    if (mqs_.active) {
        if (e->grade != mqs_.grade) {
            LOG_ERROR("CompleteMonQuest:%s - qItem.grade != mqs.grade - %d, %d", char_name_, e->grade, mqs_.grade);
            return -1;
        }
        if (mqs_.kills <= 0 || mqs_.kills < mqs_.target || mqs_.kills < gr.cnt) {
            LOG_ERROR("CompleteMonQuest:%s - mqs.killCount:%d is illegal. mqs.targetcount:%d, killcountinfo:%d",
                      char_name_, mqs_.kills, mqs_.target, gr.cnt);
            close();
            return -1;
        }
    }
    if (e->kills <= 0 || e->kills < gr.cnt) {
        LOG_ERROR("CompleteMonQuest:%s - qitem.killCount:%d is illegal. targetkillcountinfo:%d", char_name_, e->kills,
                  gr.cnt);
        close();
        return -1;
    }
    const MonsterQuestResult* res = d->result_of(e->grade);   // sub_60C9D0
    if (!res) return -1;
    std::vector<QuestItem> rewards, extra;
    if (res->type) {
        if (res->items.empty()) return -1;               // (rand() % 0 in the retail)
        const int pick = std::rand() % static_cast<int>(res->items.size());
        rewards.push_back(res->items[static_cast<size_t>(pick)]);
        if (!res->random_tables.empty()) {
            const int k = std::rand() % static_cast<int>(res->random_tables.size());
            const RandomResultItems* t = server_.quest_db().random(res->random_tables[static_cast<size_t>(k)]);
            const RandomResultItems::Entry* r = t ? t->pick() : nullptr;
            if (!r) { LOG_ERROR("RandomResultItem Table Error! TableId:%d Player Name %s", k, char_name_); return -1; }
            extra.push_back({ r->item, r->cnt });
        }
    } else {
        rewards = res->items;
        for (int id : res->random_tables) {
            const RandomResultItems* t = server_.quest_db().random(id);
            const RandomResultItems::Entry* r = t ? t->pick() : nullptr;
            if (!r) { LOG_ERROR("RandomResultItem Table Error! TableId:%d Player Name %s", id, char_name_); return -1; }
            extra.push_back({ r->item, r->cnt });
        }
    }
    int w = 0;
    for (const std::vector<QuestItem>* v : { &rewards, &extra })
        for (const QuestItem& it : *v) {
            const ItemDef* idf = server_.item_db().get(it.item);
            w += it.cnt * (idf ? idf->weight : 0);
        }
    if (w + bag_weight_ > st_maxwt_) return 2;
    if (300 - static_cast<int>(bag_.size()) < static_cast<int>(rewards.size() + extra.size())) return 2;
    int which = 3;
    if (e->count < d->max_count) {
        const int g2 = d->grade_for(level_);             // sub_6D8850
        which = g2 < 0 ? 5 : (g2 == e->grade ? 1 : 4);
    }
    int msg = -1;
    if (!monq_arg(args, size, cnt, which, msg)) return -1;

    // hItemCompleteQuest (sub_7EBE90) -> uspm_quest_completed(2, qid, the grade's monster, kills, record)
    const int mon = gr.src_monster;
    const int err = db_quest_completed(kSqlMonster, qid, mon, e->kills, e->record, history);
    if (err) {
        LOG_ERROR("QuestStore::CompletedQuest SP failure : spRet:%d, chr:%u, qType:%d, qid:%d, mid:%d", err, char_uid_,
                  kSqlMonster, qid, mon);
        send_npc_cancel();
        return -2;
    }
    // The bag records (sub_6DC0E0 with no item taken).
    std::vector<QuestItem> adds = rewards;
    adds.insert(adds.end(), extra.begin(), extra.end());
    const std::vector<BagRecord> applied = quest_bag_records({}, adds);
    pet_speech(15);      // sub_77ED80 (every OnOk): our pet's line — sub_77E2F0's own 13 then never gets in
    // sub_77E2F0: the money (S2C 41), the grade's EXP / TM EXP (sub_77E220), the entry completed, S2C 219.
    const int32_t gelder = res->gelder;
    if (gelder > 0) {
        money_ += gelder;
        send_money();
    }
    const int32_t exp = gr.exp, tmxp = gr.tmxp;
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
    {
        PacketBuffer p(static_cast<Opcode>(0x00DB), is_paula());   // S2C 219 (sub_5FA3E0)
        p.write<uint16_t>(static_cast<uint16_t>(applied.size()));
        for (const BagRecord& r : applied) write_bag_record(p, r);
        p.write<uint16_t>(static_cast<uint16_t>(adds.size()));
        for (const QuestItem& it : adds) {
            p.write<uint32_t>(it.item);
            p.write<uint16_t>(static_cast<uint16_t>(it.cnt));
        }
        p.write<uint32_t>(static_cast<uint32_t>(exp));
        p.write<uint32_t>(static_cast<uint32_t>(tmxp));
        p.write<uint16_t>(static_cast<uint16_t>(e->qid));
        p.write<uint32_t>(static_cast<uint32_t>(gelder));
        send(p);
    }
    save_position();
    LOG_INFO("GameClientSession %u: monster quest %d completed (run %d, grade %d%s): %zu records, EXP %d, TM %d, gelder %d",
             session_id(), qid, e->count, e->grade, history ? "" : ", no history", applied.size(), exp, tmxp, gelder);
    // The common OnOk (sub_77CC60): S2C 115, then S2C 211, the entry goes, the running state stops (its
    // grade stays).
    if (msg < 512) send_npc_message(n, msg, qid);
    send_monster_quest_remove(qid);
    monq_remove(qid);
    mqs_.active    = false;
    mqs_.acc_ms    = 0;
    mqs_.remain_ms = 0;
    return -2;
}

int GameClientSession::monq_check_state(NpcEntity& n, int qid, int num, const uint8_t* args, size_t size, int cnt)
{
    if (cnt != 3 || size < 6) return -1;
    const int a2 = args[2] | (args[3] << 8);
    const int a3 = args[4] | (args[5] << 8);
    if (num > 0) {
        send_npc_message(n, db_quest_history_count(kSqlMonster, qid, 0, num) ? a2 : a3, qid);
        return -2;
    }
    const MonQuestEntry* e = monq_find(qid);
    return (e && e->success) ? 1 : 3;
}
