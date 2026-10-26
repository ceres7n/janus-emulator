#include "game_server.hpp"
#include "game_session.hpp"
#include "logger.hpp"
#include "client_text.hpp"
#include "item_wire.hpp"
#include <cstdlib>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
//  NPC talk — the retail Npc state machine (sub_71DA30), the Player's talk state 9 (sub_73D2A0),
//  the dialog packets (sub_5F8F80 / sub_5F91F0) and the NPC script (ExecuteNpcScript sub_7E85A0).
//
//  C2S 0x6F (111) [u16 npc] — Player case 111 (any state): the zone's NPC of that id (sub_60E670)
//      within 300 of us (squared distance of the current positions, sub_41ED00 / sub_41ED80), else
//      S2C 113 (empty: "REQUEST_NPC_MSG_ACK_CANCEL"). Then Player +0xC9C = the NPC, the NPC gets
//      message 111 — its PreCheck picks the first message (sub_71E9F0) and sends S2C 112 — and the
//      player enters state 9.
//  State 9 (jumptable 00751B68): C2S 0x72 (114) / 0x7F (127) / 0x123 (291) go to the NPC (no NPC:
//      S2C 116 + state 0); C2S 0x75 (117) = the dialog closed -> state 0; C2S 0xEE (a skill) ->
//      S2C 239 [u16 0]. Leaving state 9 clears +0xC9C. Every other message works as in any state.
//  The client builds the dialog from the message text: the text, then '\'-separated label / action
//  pairs (sub_C04220 / sub_C03FF0): a number = C2S 114 with that message, q<n>/m<n>/s<n> = 128 /
//  256 / 512 + n, a script function name (sub_77C720: 275 names) with ','-separated u16 arguments =
//  C2S 127 [u16 npc][u8 func][u8 n][u16 × n].
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// The NPC record's numeric field (NpcRow::num).
int row_int(const NpcRow* r, const char* key)
{
    return r ? r->num(key) : 0;
}

// sub_60CC70: the record's QuestId[i] (u16 at +8 + 2i, i = 0..4), else -1.
int quest_id(const NpcRow* r, int i)
{
    if (i < 0 || i > 4) return -1;
    return r ? r->quest_id(i) : 0;
}

// Whether one of the texts the NPC's dialog can show satisfies `pred`: its messages (S2C 112 / 115) and
// the results of the quests in its QuestId slots (the q<n> messages, normal or scenario quest). In the
// data only these carry the Give_Item and Random_Move actions (the client sends C2S 127 from that text).
template <class Pred>
bool npc_dialog_any(const NpcMsgTable* msgs, const QuestDb& quests, const NpcRow* row, Pred pred)
{
    if (msgs)
        for (const NpcMsg& m : msgs->rows)
            if (pred(m.text)) return true;
    for (int k = 0; k < 5 && row; ++k) {
        for (const QuestDef* d : { quests.get(row->quest_id(k)), quests.scenario(row->quest_id(k)) }) {
            if (!d) continue;
            for (const QuestResult& res : d->results)
                for (const std::string& t : res.msgs)
                    if (pred(t)) return true;
        }
    }
    return false;
}

// Whether `text` holds the action "Random_Move,a,b,c,d,e" with exactly the five zones of `args` ([u16 × 5]).
bool text_offers_random_move(const std::string& text, const uint8_t* args)
{
    static const char kTag[] = "Random_Move,";
    for (size_t at = text.find(kTag); at != std::string::npos; at = text.find(kTag, at + 1)) {
        const char* s = text.c_str() + at + sizeof(kTag) - 1;
        int i = 0;
        for (; i < 5; ++i) {
            char* end = nullptr;
            const long v = std::strtol(s, &end, 10);
            if (end == s || v != (args[2 * i] | (args[2 * i + 1] << 8))) break;
            s = end;
            if (i < 4) {
                if (*s != ',') break;
                ++s;
            }
        }
        if (i == 5) return true;
    }
    return false;
}

// The C2S 127 argument stream (the packet reader the script gets by value, sub_708460).
struct ArgReader {
    const uint8_t* p = nullptr;
    size_t n = 0, pos = 0;
    size_t left() const { return n - pos; }
    bool u8(uint8_t& v)   { if (left() < 1) return false; v = p[pos++]; return true; }
    bool u16(uint16_t& v) { if (left() < 2) return false; v = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8)); pos += 2; return true; }
};

}  // namespace

const NpcMsgTable* GameClientSession::npc_messages(const NpcEntity& n) const
{
    const std::string* t = n.row ? n.row->get("MsgTable") : nullptr;
    return t ? server_.npc_msg_db().table(*t) : nullptr;
}

// S2C 112 (sub_5F8F80): [u16 npc][u16 npc type (+436)][u8 the message count][cstr message `index`
// — u8 0 when the list lacks it ("SendCmd_NpcMessage:: NotFound msg")] (only [u8 0] when the NPC has
// no message list) [u16 SubType (+128)][u16 ShowboardID (+132)]. VERIFIED in the client (local
// player sub_6B4E00 case 0x70): same reads; SubType 0/3/4 opens the dialog (sub_9A9D00), 1 / 2 the
// showboard (ShowboardID); the avatar enters state 15.
void GameClientSession::send_npc_talk(const NpcEntity& n, int index)
{
    PacketBuffer p(static_cast<Opcode>(0x0070), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(n.object_id));
    p.write<uint16_t>(static_cast<uint16_t>(n.row ? n.row->type : n.species));
    const NpcMsgTable* msgs = npc_messages(n);
    if (msgs && !msgs->rows.empty()) {
        p.write<uint8_t>(static_cast<uint8_t>(msgs->rows.size()));
        if (const NpcMsg* m = msgs->find(index)) {
            const std::string t = to_client_text(m->text);
            p.write_bytes(t.c_str(), t.size() + 1);
        } else {
            LOG_ERROR("SendCmd_NpcMessage:: NotFound msg : npcType : %d, msgIndex : %d", n.row ? n.row->type : 0, index);
            p.write<uint8_t>(0);
        }
    } else {
        p.write<uint8_t>(0);
    }
    p.write<uint16_t>(static_cast<uint16_t>(row_int(n.row, "SubType")));
    p.write<uint16_t>(static_cast<uint16_t>(row_int(n.row, "ShowboardID")));
    send(p);
}

// S2C 115 / 116 (sub_5F91F0): [u16 npc][u16 index] then, the message `index` of the NPC's list
// found, [cstr text][u16 NpcSType][u16 qid] -> 115; else S2C 116 with the same two words ("NPC MSG
// ACK CANCEL"). VERIFIED in the client: case 115 reads u16, u16, str, u16, u16 and shows the text
// (sub_9A9F80); case 116 closes the dialog and sets the avatar's state 0.
// Indices 128..255 (q<n>) are the message n of the quest qid's result for our run (the NPC's QuestInfo
// whose Id is qid, sub_60CA40; the run of the entry in progress, else 0 — the first result row):
// [text][u16 0][u16 qid]. (qids 1000..1022 / 1077..1999 and the NPC's ScenarioQuestInfo, and 256..511
// — a monster quest's grade messages — come with those quests.)
void GameClientSession::send_npc_message(const NpcEntity& n, int index, int qid)
{
    PacketBuffer p(static_cast<Opcode>(0x0073), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(n.object_id));
    p.write<uint16_t>(static_cast<uint16_t>(index));
    const NpcMsgTable* msgs = npc_messages(n);
    if (!msgs) {
        LOG_INFO("-- REQUEST_NPC_MSG_ACK_CANCEL(SendCmd_NpcMessage-!ni.pMsgList):player(%s), npcID(%u), "
                 "msgIndex(%d), qid(%d)", char_name_, static_cast<unsigned>(n.object_id), index, qid);
        PacketBuffer c(static_cast<Opcode>(0x0074), is_paula());
        c.write<uint16_t>(static_cast<uint16_t>(n.object_id));
        c.write<uint16_t>(static_cast<uint16_t>(index));
        send(c);
        return;
    }
    if (const NpcMsg* m = msgs->find(index)) {
        const std::string t = to_client_text(m->text);
        p.write_bytes(t.c_str(), t.size() + 1);
        p.write<uint16_t>(static_cast<uint16_t>(m->stype));
        p.write<uint16_t>(static_cast<uint16_t>(qid));
        send(p);
        return;
    }
    if (index >= 128 && index < 256) {
        const bool scenario = (qid >= 1000 && qid < 1023) || (qid >= 1077 && qid < 2000);
        const QuestDef* d = npc_quest_def(n, qid);
        QuestEntry* inv = quests_;
        if (!d || scenario) {
            // sub_60CBA0: the NPC's ScenarioQuestInfo (Npc +1732) whose Id is qid, the run from +3112
            d = nullptr;
            inv = squests_;
            for (int k = 0; k < 5 && n.row; ++k) {
                const QuestDef* s = server_.quest_db().scenario(n.row->quest_id(k));
                if (s && static_cast<int16_t>(s->id) == static_cast<int16_t>(qid)) { d = s; break; }
            }
        }
        if (d) {
            const QuestEntry* e = qinv_find(inv, d->id); // sub_60CA10: the run in progress, else 0
            const QuestResult* r = d->result_for(e ? e->count : 0);   // sub_60C950
            const int i = index - 128;
            if (r && i >= 0 && i < static_cast<int>(r->msgs.size()) && !r->msgs[static_cast<size_t>(i)].empty()) {
                const std::string t = to_client_text(r->msgs[static_cast<size_t>(i)]);
                p.write_bytes(t.c_str(), t.size() + 1);
                p.write<uint16_t>(0);
                p.write<uint16_t>(static_cast<uint16_t>(qid));
                send(p);
                return;
            }
        }
    } else if (index >= 256 && index < 512) {
        // m<n>: the message n of the NPC's MonsterQuestInfo qid (sub_60CAF0), of the result of the grade the
        // running state holds (Player +3764 — kept after the quest ends; sub_60C990 / sub_60C9D0).
        if (const MonsterQuestDef* d = npc_monster_quest_def(n, qid)) {
            const MonsterQuestResult* r = d->result_of(mqs_.grade);
            const int i = index - 256;
            if (r && i >= 0 && i < static_cast<int>(r->msgs.size()) && !r->msgs[static_cast<size_t>(i)].empty()) {
                const std::string t = to_client_text(r->msgs[static_cast<size_t>(i)]);
                p.write_bytes(t.c_str(), t.size() + 1);
                p.write<uint16_t>(0);
                p.write<uint16_t>(static_cast<uint16_t>(qid));
                send(p);
                return;
            }
        }
    }
    PacketBuffer c(static_cast<Opcode>(0x0074), is_paula());
    c.write<uint16_t>(static_cast<uint16_t>(n.object_id));
    c.write<uint16_t>(static_cast<uint16_t>(index));
    send(c);
    LOG_INFO("-- REQUEST_NPC_MSG_ACK_CANCEL(SendCmd_NpcMessage-index range over):player(%s), npcID(%u), "
             "msgIndex(%d), qid(%d)", char_name_, static_cast<unsigned>(n.object_id), index, qid);
}

// Leave state 9 (sub_414B50(player, 0) from the talk state; its exit clears +0xC9C).
void GameClientSession::npc_end_talk()
{
    talk_npc_ = 0;
}

// C2S 0x6F (Player case 111).
void GameClientSession::handle_npc_talk(PacketBuffer& pkt)
{
    try {
        const uint16_t id = pkt.read<uint16_t>();
        if (dead_ || leaving_) return;
        NpcEntity* n = server_.zone_npc(zone_id_, id);
        if (!n) {
            LOG_INFO("-- REQUEST_NPC_MSG_ACK_CANCEL(pNpc is NULL) player(%s), npc(%u)", char_name_, id);
            PacketBuffer c(static_cast<Opcode>(0x0071), is_paula());
            send(c);
            return;
        }
        int32_t px, py;
        cur_pos(std::chrono::steady_clock::now(), px, py);
        const double dx = static_cast<double>(n->pos_x) - px, dy = static_cast<double>(n->pos_y) - py;
        const int d2 = static_cast<int>(static_cast<float>(dx * dx + dy * dy));   // sub_41ED00 -> __ftol2
        if (d2 > 300 * 300) {                                                    // sub_41ED80(d2, 300)
            PacketBuffer c(static_cast<Opcode>(0x0071), is_paula());
            send(c);
            return;
        }
        talk_npc_ = static_cast<uint16_t>(n->object_id);
        rest_state_ = 0;                                  // the talk state 9 replaces sitting / resting
        shop_ = 0;                                        // and the shop state 5
        exchange_ = false;                                // and the exchange shop state 18
        drill_ = Drill{};                                 // and the drill state 3
        wh_leave_state();                                 // and the warehouse state 10
        compound_leave();                                 // and the compound state 13
        refine_ui_leave();                                // and the refine / growth / reset states
        skill_shop_leave();                               // and the skill master state 25
        // The NPC's message 111 (sub_71DA30): its PreCheck picks the first message (sub_71E9F0);
        // none (-1) -> nothing is sent. Then a hidden player (+452 bit 0) leaves its hide (vtbl+60(1286)).
        const int index = npc_precheck(*n);
        if (index >= 0) send_npc_talk(*n, index);
        hide_break();
        LOG_DEBUG("GameClientSession %u: talk to NPC %u (%s, type %d) -> message %d", session_id(), id,
                  n->name.c_str(), n->row ? n->row->type : 0, index);
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 0x6F NPC talk aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 0x6F NPC talk aborted: unknown exception", session_id());
    }
}

// The NPC's first message (Npc sub_71E9F0, switch on its PreCheckType +184; Arg +188, Yes +192,
// No +196). -1 = no message now.
int GameClientSession::npc_precheck(const NpcEntity& n)
{
    const NpcRow* r = n.row;
    const int type = row_int(r, "PreCheckType");
    const int arg  = row_int(r, "PreCheckArg");
    const int yes  = row_int(r, "PreCheckYes");
    const int no   = row_int(r, "PreCheckNo");
    switch (type) {
    case 1:   // sub_71E720: the quest Arg done at least once — hItemCheckQuestState with the PreCheck flag
    case 6:   // sub_71E960: the same for a scenario quest (type 8, SQL 6)
              // (uspm_select_quest_history_check_count(type, Arg, 0, 1): S2C 112 with Yes, else No)
        if (!arg) return no;
        return db_quest_history_count(type == 1 ? 1 : 6, arg, 0, 1) ? yes : no;
    case 2:   // sub_71E630: QuestInfo has Arg and it is in progress (the normal quests +2212, the skill
              // quests +2472 — none of those here)
        if (!arg || !server_.quest_db().get(arg)) return no;
        return quest_find(arg) ? yes : no;
    case 7:   // sub_71E8B0: ScenarioQuestInfo has Arg and it is in progress (+3112)
        if (!arg || !server_.quest_db().scenario(arg)) return no;
        return qinv_find(squests_, arg) ? yes : no;
    case 3:   // sub_71E540: an item Arg in the bag (sub_6DCC50 on +1716)
        if (!arg) return no;
        return bag_has_item(static_cast<uint32_t>(arg)) ? yes : no;
    case 4:   // sub_71E5C0: Arg == the player's FType (+1028)
        if (!arg) return no;
        return arg == f_type_ ? yes : no;
    case 5: { // sub_71E7B0: NpcCheckLvMsg Arg — the first range holding our level, else RejectMsg
        if (!arg) return no;
        const NpcCheckLvMsg* c = server_.npc_check_lv_msg_db().get(arg);
        if (!c || c->id != arg) return no;
        for (int i = 0; i < 6; ++i)
            if (c->min[i] && c->max[i] && c->min[i] <= level_ && c->max[i] >= level_) return c->msg[i];
        return c->reject_msg;
    }
    case 8:   // sub_71EB50: PreCheckSkill  } not done: the NPC answers nothing
    case 9:   // sub_71EDE0: PreCheckInfo   }
    case 10:  // sub_71F370: a guild check
        LOG_INFO("NPC %u: PreCheckType %d (arg %d) not done", static_cast<unsigned>(n.object_id), type, arg);
        return -1;
    default:
        return 0;
    }
}

// C2S 0x72 (state 9 -> the NPC's message 114): [u16 npc][u8 index]. Another NPC's id -> the
// player is dropped (sub_8AA290); else S2C 115/116 with the record's QuestId[0].
void GameClientSession::handle_npc_next(PacketBuffer& pkt)
{
    try {
        const uint16_t id = pkt.read<uint16_t>();
        const uint8_t  index = pkt.read<uint8_t>();
        if (!talk_npc_) return;                       // not in state 9: nobody takes the message
        NpcEntity* n = server_.zone_npc(zone_id_, talk_npc_);
        if (!n) { PacketBuffer c(static_cast<Opcode>(0x0074), is_paula()); send(c); npc_end_talk(); return; }
        if (n->object_id != id) {
            LOG_ERROR("NPC::CMD_REQUEST_NEXT_NPC_MESSAGE - npc is illegal: player:%s, index:%d, npcid of msg:%d, "
                      "real id:%u", char_name_, index, id, static_cast<unsigned>(n->object_id));
            close();
            return;
        }
        send_npc_message(*n, index, quest_id(n->row, 0));
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 0x72 NPC message aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 0x72 NPC message aborted: unknown exception", session_id());
    }
}

// C2S 0x75 (state 9 case 117): the dialog closed -> state 0.
void GameClientSession::handle_npc_end(PacketBuffer& /*pkt*/)
{
    if (talk_npc_) npc_end_talk();
}

// C2S 0x7F (state 9 -> the NPC's message 127): [u16 npc][u8 func][u8 n][u16 × n]. The script's
// result r: -1 = error (S2C 116), -2 / 0 = nothing more, r >= 1 = the r-th of the remaining u16
// arguments is the next message: S2C 115 with QuestId[the function's quest slot] (r >= 512: a skill
// message, not done).
void GameClientSession::handle_npc_script(PacketBuffer& pkt)
{
    try {
        const uint16_t id   = pkt.read<uint16_t>();
        const uint8_t  func = pkt.read<uint8_t>();
        if (!talk_npc_) return;
        NpcEntity* n = server_.zone_npc(zone_id_, talk_npc_);
        if (!n) { PacketBuffer c(static_cast<Opcode>(0x0074), is_paula()); send(c); npc_end_talk(); return; }
        if (n->object_id != id) {
            LOG_ERROR("NPC::CMD_REQUEST_NEXT_NPC_SCRIPT - npc is illegal: player:%s, func:%d, npcid of msg:%d, "
                      "real id:%u", char_name_, func, id, static_cast<unsigned>(n->object_id));
            close();
            return;
        }
        int quest_slot = 0;
        const int res = npc_script(*n, func, pkt.data() + pkt.rpos(),
                                   pkt.size() > pkt.rpos() ? pkt.size() - pkt.rpos() : 0, quest_slot);
        if (res < 1) {
            if (res == -1) {
                LOG_INFO("NPC::CMD_REQUEST_NEXT_NPC_SCRIPT-(resMsg==eNSRes_Error) name(%s), func(%d), npcid(%d)",
                         char_name_, func, id);
                PacketBuffer c(static_cast<Opcode>(0x0074), is_paula());
                send(c);
            }
            return;
        }
        if (quest_slot <= -1) {
            PacketBuffer c(static_cast<Opcode>(0x0074), is_paula());
            send(c);
            return;
        }
        if (res < 512) send_npc_message(*n, res, quest_id(n->row, quest_slot));
        else {
            LOG_INFO("NPC script func %d: skill message %d not done", func, res);
            PacketBuffer c(static_cast<Opcode>(0x0074), is_paula());
            send(c);
        }
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 0x7F NPC script aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 0x7F NPC script aborted: unknown exception", session_id());
    }
}

// ExecuteNpcScript (sub_7E85A0): the function `func` with the argument stream [u8 n][u16 × n];
// returns its result as the message to show (>= 1), 0 / -2 (nothing more) or -1 (error).
int GameClientSession::npc_script(NpcEntity& n, int func, const uint8_t* data, size_t size, int& quest_slot)
{
    LOG_DEBUG("ExecuteNpcScript - CHRUID:%u, Name:%s, npcType:%d, funcId:%d", char_uid_, char_name_,
              n.row ? n.row->type : 0, func);
    ArgReader a{ data, size, 0 };
    uint8_t cnt = 0;
    a.u8(cnt);
    quest_slot = 0;
    int r = -1;
    const uint8_t* qargs = a.p + a.pos;                 // the quest functions read their own copy
    const size_t   qsize = a.left();
    switch (func) {
    case 1:   // Exit (sub_7E0850): S2C 117 + state 0
        exit_npc_dialog();
        r = 0;
        break;
    case 2: case 90: case 91: case 92: case 93: case 94: case 95:      // Quest_Check(_1.._6), sub_7E0950
        quest_slot = func == 2 ? 0 : func - 89;
        r = quest_check(n, quest_id(n.row, quest_slot), qargs, qsize, cnt);
        break;
    case 3: case 96: case 97: case 98: case 99:                         // Quest_Accept(_1.._4), sub_7E0DA0
        quest_slot = func == 3 ? 0 : func - 95;
        r = quest_accept(n, quest_id(n.row, quest_slot), qargs, qsize, cnt, false);
        break;
    case 4: case 100: case 101: case 102: case 103:                     // Quest_Complete(_1.._4), sub_7E0F10
        quest_slot = func == 4 ? 0 : func - 99;
        r = quest_complete(n, quest_id(n.row, quest_slot), qargs, qsize, cnt, true);
        break;
    case 153: case 154: case 155: case 156: case 157:                   // CheckPartyQuest(_1.._5), sub_7E1B20
        quest_slot = func - 153;
        r = pq_check(n, quest_id(n.row, quest_slot));
        break;
    case 158: case 159: case 160: case 161: case 162:                   // AcceptPartyQuest, sub_7E1CC0
        quest_slot = func - 158;
        r = pq_accept(n, quest_id(n.row, quest_slot), qargs, qsize, cnt);
        break;
    case 163: case 164: case 165: case 166: case 167:                   // CompletePartyQuest, sub_7E1F00
        quest_slot = func - 163;
        r = pq_complete(n, quest_id(n.row, quest_slot), qargs, qsize, cnt);
        break;
    case 181:                                                           // Quest_NH_Complete: no history
        r = quest_complete(n, quest_id(n.row, 0), qargs, qsize, cnt, false);
        break;
    case 5: case 106: case 107: case 108: case 109:                     // Quest_Continue(_1.._4), sub_7E16E0
        quest_slot = func == 5 ? 0 : func - 105;
        r = quest_accept(n, quest_id(n.row, quest_slot), qargs, qsize, cnt, false) == -2 ? -2 : -1;
        break;
    case 6:                                                             // Quest_Quit: the first argument
        r = 1;
        break;
    case 74: case 75: case 76: case 77:                                 // FType_P/M/S/C_Only (sub_7E5400)
        r = (f_type_ == func - 74) ? 1 : 2;
        break;
    case 119:                                                           // Give_Item (sub_7E61D0)
        // Hardening (not retail): retail gives the GiveItemID to any func 119 call; 13 NPC records have one
        // that no dialog offers (e.g. Rosaline's Advice 742), so only a forged C2S 127 could farm it there.
        if (!npc_dialog_any(npc_messages(n), server_.quest_db(), n.row,
                            [](const std::string& t) { return t.find("Give_Item,") != std::string::npos; })) {
            LOG_INFO("GameClientSession %u: Give_Item refused (not offered by npc type %d)", session_id(),
                     n.row ? n.row->type : 0);
            r = -1;
            break;
        }
        r = npc_give_item(n, qargs, qsize);
        break;
    case 120:                                                           // Take_Item (sub_7E63A0)
        r = npc_take_item(n, qargs, qsize);
        break;
    case 121:                                                           // Random_Move (sub_7E6530)
        // Hardening (not retail): retail moves the player to any of the five zones the client names (from
        // any NPC); a real client only sends the zones of a "Random_Move,..." action this NPC's dialog shows.
        if (qsize >= 10 && !npc_dialog_any(npc_messages(n), server_.quest_db(), n.row,
                                           [&](const std::string& t) { return text_offers_random_move(t, qargs); })) {
            LOG_INFO("GameClientSession %u: Random_Move refused (zones not offered by npc type %d)", session_id(),
                     n.row ? n.row->type : 0);
            r = -1;
            break;
        }
        r = npc_random_move(qargs, qsize);               // (fewer than five arguments: -1 there)
        break;
    case 88: case 110: case 227: {                                      // Limit_Teleport(_50/_30) (sub_7E5990)
        uint16_t tidx = 0;
        a.u16(tidx);                                     // (read off the call's own stream)
        r = npc_limit_teleport(n, tidx, a.p + a.pos, a.left(), func == 88 ? 300 : (func == 110 ? 50 : 30));
        break;
    }
    case 82: case 104: case 105: {                                      // CheckQuestState(_1/_2), sub_7E5420
        static const char* kId[3]  = { "CheckQuestId", "CheckQuestId1", "CheckQuestId2" };
        static const char* kNum[3] = { "CheckQuestNum", "CheckQuestNum1", "CheckQuestNum2" };
        quest_slot = func == 82 ? 0 : func - 103;
        r = quest_check_state(n, static_cast<int16_t>(row_int(n.row, kId[quest_slot])),
                              static_cast<int16_t>(row_int(n.row, kNum[quest_slot])), qargs, qsize, cnt);
        break;
    }
    case 114: case 115: case 116: {                                     // ActiveQuestID_0..2, sub_7E5E10
        static const char* kKeys[3] = { "ActiveQuestID0", "ActiveQuestID1", "ActiveQuestID2" };
        r = quest_active(row_int(n.row, kKeys[func - 114]));
        break;
    }
    case 184: case 202: case 203: case 204: case 205:                   // Quest_Once_Accept(_1.._4), sub_7E2210
        quest_slot = func == 184 ? 0 : func - 201;
        r = quest_accept(n, quest_id(n.row, quest_slot), qargs, qsize, cnt, true);
        break;
    // Scenario quests (quest.cpp).
    case 122: case 123: case 124: case 125: case 126:                   // S_Quest_Check(_1.._4), sub_7E6910
        quest_slot = func - 122;
        r = squest_check(n, quest_id(n.row, quest_slot), qargs, qsize, cnt);
        break;
    case 127: case 128: case 129: case 130: case 131:                   // S_Quest_Accept(_1.._4), sub_7E6C80
        quest_slot = func - 127;
        r = squest_accept(n, quest_id(n.row, quest_slot), qargs, qsize, cnt);
        break;
    case 132: case 133: case 134: case 135: case 136:                   // S_Quest_Complete(_1.._4), sub_7E6DF0
        quest_slot = func - 132;
        r = squest_complete(n, quest_id(n.row, quest_slot), qargs, qsize, cnt);
        break;
    case 137: case 138: case 139: case 140: case 141:                   // S_Quest_Continue(_1.._4), sub_7E7150
        quest_slot = func - 137;
        r = squest_accept(n, quest_id(n.row, quest_slot), qargs, qsize, cnt) == -2 ? -2 : -1;
        break;
    case 142: case 143: case 144: {                                     // S_ActiveQuestID_0..2, sub_7E7180
        static const char* kKeys[3] = { "ActiveQuestID0", "ActiveQuestID1", "ActiveQuestID2" };
        const int q = row_int(n.row, kKeys[func - 142]);
        r = !server_.quest_db().scenario(q) ? 2 : (qinv_find(squests_, q) ? 1 : 2);
        break;
    }
    case 145: case 146: case 147: {                                    // S_CheckQuestState(_1/_2), sub_7E71D0
        static const char* kId[3]  = { "CheckQuestId", "CheckQuestId1", "CheckQuestId2" };
        static const char* kNum[3] = { "CheckQuestNum", "CheckQuestNum1", "CheckQuestNum2" };
        quest_slot = func - 145;
        r = quest_check_state(n, static_cast<int16_t>(row_int(n.row, kId[quest_slot])),
                              static_cast<int16_t>(row_int(n.row, kNum[quest_slot])), qargs, qsize, cnt, 6);
        break;
    }
    // Monster quests (monster_quest.cpp).
    case 7: case 238: case 239: case 240: case 241:                     // MonQuest_Check(_1.._4), sub_7E1720
        quest_slot = func == 7 ? 0 : func - 237;
        r = monq_check(n, quest_id(n.row, quest_slot), qargs, qsize, cnt);
        break;
    case 8: case 242: case 243: case 244: case 245:                     // MonQuest_Accept(_1.._4), sub_7E2010
        quest_slot = func == 8 ? 0 : func - 241;
        r = monq_accept(n, quest_id(n.row, quest_slot), qargs, qsize, cnt, false);
        break;
    case 9: case 246: case 247: case 248: case 249:                     // MonQuest_Complete(_1.._4), sub_7E2680
        quest_slot = func == 9 ? 0 : func - 245;
        r = monq_complete(n, quest_id(n.row, quest_slot), qargs, qsize, cnt, true);
        break;
    case 149:                                                           // MonQuest_NH_Complete: no history
        r = monq_complete(n, quest_id(n.row, 0), qargs, qsize, cnt, false);
        break;
    case 10: case 250: case 251: case 252: case 253:                    // MonQuest_Continue(_1.._4), sub_7E2C10
        quest_slot = func == 10 ? 0 : func - 249;
        r = monq_accept(n, quest_id(n.row, quest_slot), qargs, qsize, cnt, false) == -2 ? -2 : -1;
        break;
    case 11:                                                            // MonQuest_Quit: the first argument
        r = 1;
        break;
    case 12:                                                            // MonQuest_AllRank (sub_7E2C50: S2C 7509)
    case 13:                                                            // MonQuest_ZoneRank (sub_7E2C80: S2C 7510)
        // (the ranking windows are not done; the first argument's message still comes)
        r = 1;
        break;
    case 185:                                                           // MonQuest_Once_Accept, sub_7E2400
        r = monq_accept(n, quest_id(n.row, 0), qargs, qsize, cnt, true);
        break;
    case 83:                                                            // CheckMonQuestState, sub_7E5520
        r = monq_check_state(n, row_int(n.row, "CheckMonQuestId"), row_int(n.row, "CheckMonQuestNum"), qargs, qsize,
                             cnt);
        break;
    case 14: {   // Move_Check (sub_7E2CC0): the NPC's NpcMoveInfo (NpcMoveId +20); none -> -1;
                 // its check (sub_6D85F0) 0..4 -> 1..5
        const NpcMoveInfo* mi = server_.npc_move_db().get(row_int(n.row, "NpcMoveId"));
        r = mi ? npc_move_check(*mi) + 1 : -1;
        break;
    }
    case 15: {   // Move_Complete (sub_7E2D40); with TakeItem it reads the first argument itself
        ArgReader c = a;                             // (sub_7EC100(stream, 1)) and answers -2
        uint16_t first = 0;
        const int first_arg = c.u16(first) ? first : -1;
        r = npc_move_complete(n, first_arg);
        break;
    }
    case 18: {   // Teleport_Check (sub_7E31E0): the slot argument
        uint16_t tidx = 0;
        a.u16(tidx);
        r = npc_teleport_check(n, tidx);
        break;
    }
    case 19: {   // Teleport_Start (sub_7E3280 with no image)
        uint16_t tidx = 0;
        a.u16(tidx);
        r = npc_teleport_start(n, tidx);
        break;
    }
    case 16:     // Warehouse_Start (sub_7E2E30(player, 1), warehouse.cpp): 0 / 2 / 3
        r = npc_warehouse_start(1);
        break;
    case 17:     // NeoWarehouse_Start (sub_7E3060, bank.cpp): 0 / 2 / 3
        r = npc_bank_start();
        break;
    case 20:     // Compound_Start (sub_7E3190, compound.cpp)
        r = npc_compound_start(n);
        break;
    case 71:     // Item_Refine (sub_7E5380, refine.cpp): state 30 + S2C 710
        r = npc_refine_start();
        break;
    case 72:     // Item_Growth (sub_7E53A0, refine.cpp): state 31 + S2C 711
        r = npc_growth_start();
        break;
    case 73:     // Reset_Mix (sub_7E53C0, refine.cpp): state 32 + S2C 739
        r = npc_resetmix_start();
        break;
    case 22:     // ExchangeShop_Start (sub_7E3410, exchange_shop.cpp)
        r = npc_exchange_shop_start(n);
        break;
    case 84: {   // CheckHaveItem (sub_7E5860): 2 args -> CheckItemId0; 4 args -> CheckItemId0 + 1
        if (a.left() != static_cast<size_t>(2 * cnt)) { r = -1; break; }
        const bool i0 = bag_has_item(static_cast<uint32_t>(row_int(n.row, "CheckItemId0")));
        if (cnt == 2) { r = i0 ? 1 : 2; break; }
        if (cnt == 4) {
            const bool i1 = bag_has_item(static_cast<uint32_t>(row_int(n.row, "CheckItemId1")));
            r = i0 ? (i1 ? 1 : 2) : (i1 ? 3 : 4);
            break;
        }
        r = -1;
        break;
    }
    case 86: {   // CheckGender (sub_7E58B0): 2 args; the model (+436) 1..24: even -> 1, odd -> 2
        if (cnt != 2 || a.left() != 4) { r = -1; break; }
        if (!type_ || static_cast<uint16_t>(type_) > 24) { r = -1; break; }
        r = (type_ & 1) ? 2 : 1;
        break;
    }
    case 118: {  // Check_Job (sub_7E6110): the job's bit in JobTypeFlag1 (jobs 1..16, bit job-1)
                 // or JobTypeFlag2 (jobs 17..33, bit job-17) -> 1, else 2
        if (job_ <= 0 || job_ >= 34) { r = 2; break; }
        if (job_ >= 17) r = (row_int(n.row, "JobTypeFlag2") & (1 << (job_ - 17))) ? 1 : 2;
        else            r = (row_int(n.row, "JobTypeFlag1") & (1 << (job_ - 1))) ? 1 : 2;
        break;
    }
    case 151: {  // Random_Msg (sub_7E6830): five u16 arguments (read from its own copy of the
                 // stream), one at random
        ArgReader c = a;
        bool ok = true;
        for (int i = 0; i < 5; ++i) { uint16_t v; if (!c.u16(v)) { ok = false; break; } }
        if (!ok) { r = -1; break; }
        r = std::rand() % 5 + 1;
        break;
    }
    default:
        LOG_INFO("ExecuteNpcScript: funcId %d not done (npcType %d)", func, n.row ? n.row->type : 0);
        return -1;
    }
    if (r < 1) return r;
    if (r > cnt) {
        LOG_ERROR("%s Invalid npcScript arg cnt:funcId:%d, res:%d, cntArg:%d, npctype:%d, zoneNo:%d", char_name_,
                  func, r, cnt, n.row ? n.row->type : 0, zone_id_);
        return -1;
    }
    uint16_t v = 0;
    for (int i = 0; i < r; ++i) {
        if (!a.u16(v)) {
            LOG_ERROR("%s Invalid npcScript res:funcId:%d, res:%d, cntArg:%d, npctype:%d, zoneNo:%d, res:%d",
                      char_name_, func, r, cnt, n.row ? n.row->type : 0, zone_id_, i);
            return -1;
        }
    }
    return v;
}

// S2C 117 (empty, sub_5F3BA0) + state 0: the dialog ends (client case 117 -> sub_6DDB70).
void GameClientSession::exit_npc_dialog()
{
    PacketBuffer p(static_cast<Opcode>(0x0075), is_paula());
    send(p);
    npc_end_talk();
}

// Teleport_Check (sub_7E31E0): the NPC's TeleportInfo (TeleportId +18); none -> -1; our level
// under its MinLevel -> 2; the slot out of range -> -1; our money >= the slot's Cost -> 1, else 3.
int GameClientSession::npc_teleport_check(const NpcEntity& n, int tidx)
{
    const TeleportInfo* ti = server_.teleport_db().get(row_int(n.row, "TeleportId"));
    if (!ti) return -1;
    if (level_ < ti->min_level) return 2;
    if (tidx < 0 || tidx >= static_cast<int>(ti->slots.size())) {
        LOG_ERROR("%s: NpcScript::CheckTeleport - tidx < 0 || tidx >= ti.cntTeleport: tidx:%d", char_name_, tidx);
        return -1;
    }
    return money_ >= ti->slots[static_cast<size_t>(tidx)].cost ? 1 : 3;
}

// Teleport_Start (sub_7E3280): only after a check that answers 1. S2C 117 + state 0; a Cost is paid
// (+3148 -= Cost, money log, S2C 0x29); then the player's own message 18 [u16 zone][u8 3] (Player
// case 18: zone 1..999 -> FieldShell sub_7FDB80(zone, changeZoneType 3, pos (0,0)) -> sub_5F77A0:
// S2C 0x1B (stop) to the area, S2C 153 to us, S2C 21 to the area, and the field change; the
// destination puts us on a random type-7 point, sub_810A60 -> sub_810A10). Returns 0.
int GameClientSession::npc_teleport_start(const NpcEntity& n, int tidx)
{
    if (npc_teleport_check(n, tidx) != 1) return -1;
    const TeleportInfo* ti = server_.teleport_db().get(row_int(n.row, "TeleportId"));
    if (!ti) return -1;
    const TeleportSlot& s = ti->slots[static_cast<size_t>(tidx)];
    exit_npc_dialog();
    if (s.cost) {
        money_ -= s.cost;
        send_money();
    }
    LOG_INFO("GameClientSession %u: NPC teleport %d -> zone %d, cost %d", session_id(), ti->id, s.zone, s.cost);
    player_zone_move(s.zone);
    return 0;
}

// The player's own message 18 [u16 zone][u8 3] (Player case 18): a zone 1..999 -> FieldShell sub_7FDB80(zone,
// changeZoneType 3, pos (0, 0)) -> sub_5F77A0 (leave_field) and the field change; the destination puts us on a
// random type-7 point (sub_810A60 -> sub_810A10). An unknown zone: the Master refuses it -> 155 + drop.
void GameClientSession::player_zone_move(int zone)
{
    if (zone < 1 || zone >= 1000) return;               // case 18 ignores it
    int32_t ax = 500, ay = 400;
    if (!server_.maps_db().get(zone)) {
        leave_field(zone);
        return;
    }
    server_.maps_db().teleport_arrival(zone, ax, ay);
    LOG_INFO("GameClientSession %u: zone move -> zone %d (%d,%d)", session_id(), zone, ax, ay);
    if (leave_field(zone)) begin_field_change(static_cast<int16_t>(zone), ax, ay, 3);
}

// Give_Item (119, sub_7E61D0): no free bag entry, no GiveItemID (NpcTalk +208) or no such item, or its weight
// over MaxWT -> 2; two arguments; hNpcGiveItem (one of the item, sub_6DC000) -> OnOk (sub_78AAF0): the records,
// S2C 115 with the 1st argument (qid = QuestId[0]), S2C 586 [u16 n]{record} (sub_5FEED0; client case 586
// applies them and names the items). -2.
int GameClientSession::npc_give_item(NpcEntity& n, const uint8_t* args, size_t size)
{
    if (300 - static_cast<int>(bag_.size()) <= 0) return 2;
    const uint32_t id = static_cast<uint32_t>(row_int(n.row, "GiveItemID"));
    if (!id) return 2;
    const ItemDef* d = server_.item_db().get(id);
    if (!d) return 2;
    if (bag_weight_ + d->weight > st_maxwt_) return 2;
    if (size < 4) return -1;
    const int yes = args[0] | (args[1] << 8);
    const std::vector<BagRecord> recs = quest_bag_records({}, { QuestItem{ id, 1 } });
    if (recs.empty()) {
        LOG_INFO("hNpcGiveItem::Send - failed to PrepareToAddItems_MakeItemOps: %u,%s", static_cast<unsigned>(object_id_),
                 char_name_);
        return -2;
    }
    send_npc_message(n, yes, quest_id(n.row, 0));
    PacketBuffer p(static_cast<Opcode>(0x024A), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(recs.size()));
    for (const BagRecord& r : recs) write_bag_record(p, r);
    send(p);
    LOG_INFO("%s : hNpcGiveItem::OnOk (item %u)", char_name_, id);
    return -2;
}

// Take_Item (120, sub_7E63A0): no TakeItemID (+212) or none in the bag -> 2; two arguments; hNpcTakeItem (one of
// it, sub_6DC070) -> OnOk (sub_78ACC0): the records, S2C 586, S2C 115 with the 1st argument (qid = the NPC's
// QuestInfo[0] Id, sub_60CAA0: -1 without one). -2.
int GameClientSession::npc_take_item(NpcEntity& n, const uint8_t* args, size_t size)
{
    const uint32_t id = static_cast<uint32_t>(row_int(n.row, "TakeItemID"));
    if (!id || !bag_find_id(id)) return 2;
    if (size < 4) return -1;
    const int yes = args[0] | (args[1] << 8);
    const std::vector<BagRecord> recs = quest_bag_records({ QuestItem{ id, 1 } }, {});
    PacketBuffer p(static_cast<Opcode>(0x024A), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(recs.size()));
    for (const BagRecord& r : recs) write_bag_record(p, r);
    send(p);
    const QuestDef* q0 = n.row ? server_.quest_db().get(n.row->quest_id(0)) : nullptr;
    send_npc_message(n, yes, q0 ? static_cast<int16_t>(q0->id) : -1);
    return -2;
}

// Limit_Teleport (88 / Limit_Teleport_50 110 / Limit_Teleport_30 227, sub_7E5990): the slot `tidx` (the first
// argument, read by ExecuteNpcScript) of the NPC's TeleportInfo (none -> -1, out of range -> -1); money under
// its Cost -> 3; three more arguments; hNpcAcceptMove (sub_7EC700) asks the Master for the zone's users
// (3079) -> OnOk (sub_789E70): under the limit -> S2C 115 with the next argument, state 0, the Cost paid
// (S2C 41), message 18 (the zone change); else S2C 115 with the one after.
int GameClientSession::npc_limit_teleport(NpcEntity& n, int tidx, const uint8_t* args, size_t size, int limit)
{
    const TeleportInfo* ti = server_.teleport_db().get(row_int(n.row, "TeleportId"));
    if (!ti) return -1;
    if (tidx < 0 || tidx >= static_cast<int>(ti->slots.size())) {
        LOG_ERROR("%s: NpcScript::LimitTeleport - tidx < 0 || tidx >= ti.cntTeleport: tidx:%d", char_name_, tidx);
        return -1;
    }
    const TeleportSlot& s = ti->slots[static_cast<size_t>(tidx)];
    if (money_ < s.cost) return 3;
    if (size < 6) return -1;
    const int ok_msg   = args[0] | (args[1] << 8);
    const int full_msg = args[2] | (args[3] << 8);
    const QuestDef* q0 = n.row ? server_.quest_db().get(n.row->quest_id(0)) : nullptr;
    const int qid = q0 ? static_cast<int16_t>(q0->id) : -1;
    if (server_.zone_player_count(static_cast<int16_t>(s.zone)) >= limit) {
        send_npc_message(n, full_msg, qid);
        return -2;
    }
    send_npc_message(n, ok_msg, qid);
    npc_end_talk();                                      // sub_414B50(player, 0)
    if (s.cost) {
        money_ -= s.cost;
        send_money();
    }
    LOG_INFO("GameClientSession %u: limited teleport %d slot %d -> zone %d (limit %d), cost %d", session_id(), ti->id,
             tidx, s.zone, limit, s.cost);
    player_zone_move(s.zone);
    return -2;
}

// Random_Move (121, sub_7E6530): five zones as arguments; message 18 to one of them at random (rand() % 5). -2.
int GameClientSession::npc_random_move(const uint8_t* args, size_t size)
{
    if (size < 10) return -1;
    int zones[5];
    for (int i = 0; i < 5; ++i) zones[i] = args[2 * i] | (args[2 * i + 1] << 8);
    player_zone_move(zones[std::rand() % 5]);
    return -2;
}

// sub_5F7A20 / sub_5F77A0 (the leave of a portal / teleport): S2C 0x1B [id][cur pos][u8 state]
// (sub_5F54E0) to the area, S2C 153 [u16 zoneTo][pos 0,0] to us (the client stops its avatar,
// sub_6ABF60(0), reading no body), S2C 21 [u16 id] to the area. An unknown destination: Master's
// MCMD_LEAVE_ZONE_ACK_CANCEL -> S2C 155 (client "MBStatic_MoveMapFailed") and the client is dropped.
bool GameClientSession::leave_field(int zone_to)
{
    leaving_      = true;
    portal_armed_ = false;
    end_attack(true);
    end_all_mob_attacks(true);                      // we leave: the monsters lose their target
    pick_ = Pickup{};
    drill_ = Drill{};
    exchange_ = false;
    {
        // Client::PrepareLeavingZone (sub_40B800 -> sub_7B05E0): the Crazy Drill state goes along.
        cGameServer::CdmCarry c;
        c.active       = cdm_.active;
        c.drill_uid    = cdm_.drill_uid;
        c.remain_ms    = cdm_.remain_ms;
        c.half_ms      = cdm_.half_ms;
        c.quarter_ms   = cdm_.quarter_ms;
        c.warn_half    = cdm_.warn_half;
        c.warn_quarter = cdm_.warn_quarter;
        c.warn_5s      = cdm_.warn_5s;
        c.gauge        = cdm_.gauge;
        server_.cdm_put(char_uid_, c);
    }
    monq_carry_out();                               // and the monster quest (sub_40B740 / sub_75E9D0)
    pq_carry_out();                                 // and the party quest's entry (party_quest.cpp)
    npc_end_talk();
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    const uint16_t self_eid = static_cast<uint16_t>(object_id_);
    {
        PacketBuffer s(static_cast<Opcode>(0x001B), is_paula());
        s.write<uint16_t>(self_eid);
        s.write<uint16_t>(static_cast<uint16_t>(cx));
        s.write<uint16_t>(static_cast<uint16_t>(cy));
        s.write<uint8_t>(static_cast<uint8_t>(motion_.state_flags(now)));
        server_.broadcast_zone(zone_id_, s, 0);
    }
    motion_.rest(cx, cy);
    {
        PacketBuffer a(static_cast<Opcode>(0x0099), is_paula());
        a.write<uint16_t>(static_cast<uint16_t>(zone_to));
        a.write<uint16_t>(0);
        a.write<uint16_t>(0);
        send(a);
    }
    {
        PacketBuffer g(static_cast<Opcode>(0x0015), is_paula());
        g.write<uint16_t>(self_eid);
        server_.broadcast_zone(zone_id_, g, 0);
    }
    pet_destroy(false);                             // our pet's owner ref goes: it leaves too (pet.cpp)
    if (trade_ || trade_opposite_ || trade_id_ >= 0) { trade_reset(); trade_leave(); }   // and a trade (trade.cpp)
    if (!server_.maps_db().get(zone_to)) {
        LOG_ERROR("GameClientSession %u: zone %d -> unknown zone %d: MCMD_LEAVE_ZONE_ACK_CANCEL -> 155 + drop",
                  session_id(), zone_id_, zone_to);
        PacketBuffer f(static_cast<Opcode>(0x009B), is_paula());
        send(f);
        close();
        return false;
    }
    return true;
}

// NpcMoveInfo check (sub_6D85F0): 0 = ok; 1 = the items (sub_6D85B0 -> sub_6DB140: each needed item
// is in the bag with at least its count, sub_50CD80); 2 = the level (sub_6D8AC0: Min and Max both 0,
// or Min <= level <= Max); 3 = the FType (sub_6D8B10: -1 or ours, +1028); 4 = the model (sub_6D8B40:
// 0 or ours, +436).
int GameClientSession::npc_move_check(const NpcMoveInfo& mi)
{
    for (const NpcMoveInfo::Src& s : mi.items) {
        const BagItem* b = bag_find_id(static_cast<uint32_t>(s.item));
        if (!b || b->count < s.cnt) return 1;
    }
    if ((mi.min_level || mi.max_level) && (level_ < mi.min_level || level_ > mi.max_level)) return 2;
    if (mi.ftype != -1 && mi.ftype != f_type_) return 3;
    if (mi.chr_type && mi.chr_type != type_) return 4;
    return 0;
}

// Move_Complete (sub_7E2D40): a take still waiting for the DB (+1636 flag 8) -> -1; the NpcMoveInfo;
// its check must pass (else -1). TakeItem 1: hItemAcceptMove (sub_7EBF80) — the needed items leave the
// bag (the records of sub_6DC070 / sub_6DBF00, all with an item id), the DB (4020), then OnOk
// (sub_77F6D0): the records applied (sub_6DBB80), S2C 195 [u16 n]{record}[u16 m]{u32 item, u16 count}
// (sub_5FC1D0; client case 195: sub_6B3DC0 per record, then the list), the move in 2000 ms, S2C 115
// with the message of the first argument (qid = QuestId[0]) and the talk state ends; -2. Else the
// move in 2000 ms and 1 (the first argument's message).
int GameClientSession::npc_move_complete(NpcEntity& n, int first_arg)
{
    const NpcMoveInfo* mi = server_.npc_move_db().get(row_int(n.row, "NpcMoveId"));
    if (!mi) return -1;
    if (npc_move_check(*mi) != 0) return -1;
    int32_t tx = 0, ty = 0;
    server_.maps_db().type9_point(mi->zone, mi->dest_id, tx, ty);   // GameInfoMgr: the zone's type-9 DestId
    if (mi->take_item == 1) {
        std::vector<BagRecord> recs;
        for (const NpcMoveInfo::Src& s : mi->items) {
            BagItem* b = bag_find_id(static_cast<uint32_t>(s.item));
            if (!b) {
                LOG_ERROR("hItemAcceptMove invalid itemID name : %s", char_name_);
                return -1;
            }
            recs.push_back(bag_decrease(*b, static_cast<uint16_t>(s.cnt)));
        }
        for (const BagRecord& rec : recs) {
            if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
            else               server_.item_store().SetAmount(rec.uid, rec.count);
            bag_apply(rec);
        }
        PacketBuffer p(static_cast<Opcode>(0x00C3), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(recs.size()));
        for (const BagRecord& rec : recs) {                         // sub_60C220 / sub_60C070
            p.write<uint8_t>(rec.type);
            p.write<uint8_t>(rec.kind);
            write_item_data(p, rec.kind, rec.item_id, rec.uid, rec.count, rec.rare);
        }
        p.write<uint16_t>(static_cast<uint16_t>(mi->items.size()));
        for (const NpcMoveInfo::Src& s : mi->items) {
            p.write<uint32_t>(static_cast<uint32_t>(s.item));
            p.write<uint16_t>(static_cast<uint16_t>(s.cnt));
        }
        send(p);
        npc_move_after(mi->zone, tx, ty);
        if (first_arg >= 0) send_npc_message(n, first_arg, quest_id(n.row, 0));
        npc_end_talk();
        return -2;
    }
    npc_move_after(mi->zone, tx, ty);
    return 1;
}

// The NPC move itself (sub_50D810: event 30083 to the player in 2000 ms) — the field change of
// changeZoneType 4 (sub_7FDB80 -> sub_5F77A0, leave_field) with the given position when it is valid
// (sub_819350 case 4 -> sub_810B20: x, y > 0 and its cell not blocked, sub_810620), else the
// destination's first type-9 point (sub_810AD0 -> sub_813470), else its default arrival (sub_8108F0).
void GameClientSession::npc_move_after(int zone, int32_t x, int32_t y, int delay_ms)
{
    auto self = shared_from_this();
    const int16_t from = zone_id_;
    schedule_delayed(std::chrono::milliseconds(delay_ms), [this, self, zone, x, y, from]() {
        if (state_ != GameClientSessionState::InWorld || dead_ || leaving_ || zone_id_ != from) return;
        int32_t ax = x, ay = y;
        bool ok = x > 0 && y > 0;
        if (ok) {
            const MapGrid* g = server_.zone_grid(static_cast<int16_t>(zone));
            ok = !g || g->at(x / 16, y / 16) != 1;
        }
        if (!ok) {
            const MapDef* md = server_.maps_db().get(zone);
            if (md && !md->type9.empty()) { ax = md->type9.front().x; ay = md->type9.front().y; }
            else                          server_.maps_db().default_arrival(zone, ax, ay);
        }
        LOG_INFO("GameClientSession %u: NPC move -> zone %d (%d,%d)", session_id(), zone, ax, ay);
        if (zone < 1 || zone >= 1000) return;
        if (leave_field(zone)) begin_field_change(static_cast<int16_t>(zone), ax, ay, 4);
    });
}
