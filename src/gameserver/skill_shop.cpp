#include "game_server.hpp"
#include "game_session.hpp"
#include "client_text.hpp"
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  Skill / job masters — 1:1 with the retail GameServer (GameServerRelease) and the KR client.
//
//  A map's SkillShopTable rows are SkillShopNpcs (object kind 14, cGameServer::spawn_skill_shops at the
//  .md3 type-10 points); the client sends C2S 486 when one is clicked.
//    C2S 486 [u16 master] (Player case 486, a message of every state; normal fields only): no such master
//      -> S2C 488; else Player +0xC9C = it, the master's message 486 (sub_5FD410): S2C 487 [u16 master]
//      [u16 its Type][u8 messages][cstr Message0 when any] (client avatar sub_6B4E00 case 487: the
//      window 25), the state 25. (A player in the ESAction-1286 state leaves it first — no such ES here.)
//  The state 25 (Player jumptable 0075406C / 007540B4) swallows moves (24), actions (64) and 0, answers a
//  skill use request (238) with S2C 239 [u16 0] and the skill toggles 274 / 277 with S2C 276 / 279, and
//  hands these to the master named in the message (none -> S2C 491 and state 0):
//    C2S 489 [u16 master][u8 index] -> sub_5FD580: S2C 490 [u16 master][cstr Message<index>][u16 its
//      NpcSType][u16 JobQuestID][u16 index]; an index 128..255 is the message index-128 of the
//      JobQuestID quest's result for our run of it (the skill quests +2472; none: the first row) with
//      NpcSType 0; anything else -> S2C 491.
//    C2S 496 [u16 master][u8 func][u8 n][u16 x n] -> ExecuteSkillShopNpcScript (sub_7EA770) and C2S 497
//      [u16 master][u8 func][u8 p]{[u8 kind] u32 | cstr}[u8 n][u16 x n] -> ExecuteSkillShopNpcParamScript
//      (sub_7EAB90): a result r >= 1 is the r-th argument -> that message (sub_5FD580); -1 -> S2C 491;
//      0 / -2 nothing more.
//    C2S 503 [u16 master] -> S2C 500 [u16 n]{u32 ItemId, u32 price} of its ItemTable not OutOfStock
//      (sub_5FD870; price sub_721480 = (int)(ShopPriceRate x Value x rate), not rounded); C2S 505 ->
//      S2C 501 [f32 ShopPriceRate x its SellingRate][u16 n]{u32 ItemId, f32 SellingRate > 0} (sub_5FD970);
//      no master -> S2C 504 / 506 (no client handler: not sent).
//    C2S 72 / 0x39F inner 76: buying / selling with the master (shop.cpp).
//    C2S 492 / 502 -> state 0.
//  The script functions (ids = the client's table): 1 Exit / 38 SkillShop_Exit -> S2C 492, state 0;
//  36 SkillShop_Buy / 37 SkillShop_Sell -> S2C 500 / 501; 39 SkillMaster_Check -> nothing; 59
//  SkillMasterUI -> S2C 557 [u16 n]{u16 skill} (sub_5FE5A0: our level-10 skills with a SkillMasterInfo
//  row that the master's LimitSkillMaster names); 60 MagicElementUI -> S2C 559; 64 CJUIOpen -> S2C 568
//  [u16 ChangeType][u16 ChangeJob]; 61 / 111 CJQuest(2)_Check, 62 / 112 _Accept, 63 / 113 _Complete —
//  the change-job quests (the skill quests, Player +2472, one entry; SQL type 3; S2C 294 / 295 / 296,
//  the state characters.change_job_queststate); 25 / 26 SkillQuest_Accept / _Complete (no dialog uses
//  them). The param script's 1 is the NPC Exit (S2C 117, state 0), 24 SkillQuest_Check.
//  C2S 569 the job change (the state 25) and C2S 560 the skill mastery (any state): below.
//  The jobs: the recovered EXE numbers them without the Animal Lover line (1..8 / 9..16 / 17..33); the
//  data and the client use JobTypeInfo (1..9 / 10..18 / 19..36) — its ranges are mapped (pact_skill /
//  job_tier and the checks of cj_quest_check / change_job).
//  All VERIFIED in the client (sub_6F0960 cases 488 / 490 / 491 / 492 / 500 / 501 / 557 / 559 / 568 /
//  571 / 294 / 295 / 150, the loading dispatcher's 296, the avatar's 487 / 276 / 279 / 524 / 561 / 562 /
//  564 / 570 / 582, the remote player's 482 / 570; c2s_486 / 489 / 492 / 496 / 497 / 503 / 505 / 560 /
//  569).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct SArg {
    const uint8_t* p;
    size_t size;
    size_t pos;
    bool u8(uint8_t& v) { if (pos + 1 > size) return false; v = p[pos++]; return true; }
    bool u16(uint16_t& v) { if (pos + 2 > size) return false; v = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8)); pos += 2; return true; }
    bool u32(uint32_t& v)
    {
        if (pos + 4 > size) return false;
        v = static_cast<uint32_t>(p[pos]) | (static_cast<uint32_t>(p[pos + 1]) << 8) |
            (static_cast<uint32_t>(p[pos + 2]) << 16) | (static_cast<uint32_t>(p[pos + 3]) << 24);
        pos += 4;
        return true;
    }
    bool cstr()
    {
        while (pos < size) if (p[pos++] == 0) return true;
        return false;
    }
};

// sub_7EB070 (see quest.cpp): the idx-th u16 argument, read from the start — false when the call has
// fewer than idx arguments, the stream ends first, or the value is 0.
bool sshop_arg(const uint8_t* p, size_t size, int cnt, int idx, int& out)
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

// The jobs. The recovered GameServer predates the Animal Lover line: its job ranges are 1..8 / 9..16 /
// 17..33, the data's and the client's (JobTypeInfo) 1..9 / 10..18 / 19..36 — the masters' LimitJobType /
// ChangeJob are the data's. Its ChangeJob 24 / 25 — Dark Lord / Priest (their palettes job024 / job025)
// — are the data's 26 / 27, and a change to them asks for Dark Pact 2202 / Light Pact 2204.
int pact_skill(int change_job)
{
    if (change_job == 26) return 2202;
    if (change_job == 27) return 2204;
    return 0;
}

// hItemChangeJob::Send (sub_78A0A0): the tier of the new job (the EXE: [1,9) / [9,17) / [17,34)).
int job_tier(int job)
{
    if (job >= 1 && job < 10)  return 1;
    if (job >= 10 && job < 19) return 2;
    if (job >= 19 && job < 37) return 3;
    return 0;
}

// The Master's SQL quest type of the GameServer's quest type 7 (dword_591AA4[7]).
constexpr int kSqlSkillQuest = 3;

} // namespace

void GameClientSession::skill_shop_leave()
{
    sshop_ = 0;
}

// C2S 486 (any state).
void GameClientSession::handle_skill_shop_open(PacketBuffer& pkt)
{
    try {
        const uint16_t id = pkt.read<uint16_t>();
        if (dead_ || leaving_) return;
        NpcEntity* n = server_.zone_skill_shop(zone_id_, id);
        if (!n || !n->sshop) {
            PacketBuffer f(static_cast<Opcode>(0x01E8), is_paula());   // S2C 488 (__fputchar_68)
            send(f);
            return;
        }
        rest_state_ = 0;                                 // (the state 25 replaces the others)
        shop_       = 0;
        exchange_   = false;
        drill_      = Drill{};
        wh_leave_state();
        compound_leave();
        if (talk_npc_) npc_end_talk();
        sshop_ = id;
        // the master's message 486 (sub_5FD410): S2C 487
        const SkillShopRow& r = *n->sshop;
        PacketBuffer p(static_cast<Opcode>(0x01E7), is_paula());
        p.write<uint16_t>(id);
        p.write<uint16_t>(static_cast<uint16_t>(r.type));
        p.write<uint8_t>(static_cast<uint8_t>(r.msgs.size()));
        if (!r.msgs.empty()) {
            const std::string t = to_client_text(r.msgs[0].text);
            p.write_bytes(t.c_str(), t.size() + 1);
        }
        send(p);
        LOG_DEBUG("GameClientSession %u: skill master %u (%s, %s)", session_id(), id, n->name.c_str(), r.table.c_str());
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 486 skill master open aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 486 skill master open aborted: unknown exception", session_id());
    }
}

// sub_5FD580: the master's message `index` (S2C 490), else S2C 491.
void GameClientSession::send_skill_shop_msg(const NpcEntity& n, int index)
{
    const SkillShopRow& r = *n.sshop;
    if (index >= 0 && index < static_cast<int>(r.msgs.size())) {
        PacketBuffer p(static_cast<Opcode>(0x01EA), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(n.object_id));
        const std::string t = to_client_text(r.msgs[static_cast<size_t>(index)].text);
        p.write_bytes(t.c_str(), t.size() + 1);
        p.write<uint16_t>(static_cast<uint16_t>(r.msgs[static_cast<size_t>(index)].stype));
        p.write<uint16_t>(static_cast<uint16_t>(r.job_quest_id));
        p.write<uint16_t>(static_cast<uint16_t>(index));
        send(p);
        return;
    }
    if (index >= 128 && index < 256) {
        // npc +1804 = QuestInfo(JobQuestID): the message index-128 of its result for our run (+2472).
        if (const QuestDef* q = server_.quest_db().get(r.job_quest_id)) {
            const QuestEntry* e = qinv_find(skquests_, q->id, kSkillQuestCap);
            const QuestResult* res = q->result_for(e ? e->count : 0);
            const int i = index - 128;
            if (res && i < static_cast<int>(res->msgs.size()) && !res->msgs[static_cast<size_t>(i)].empty()) {
                PacketBuffer p(static_cast<Opcode>(0x01EA), is_paula());
                p.write<uint16_t>(static_cast<uint16_t>(n.object_id));
                const std::string t = to_client_text(res->msgs[static_cast<size_t>(i)]);
                p.write_bytes(t.c_str(), t.size() + 1);
                p.write<uint16_t>(0);
                p.write<uint16_t>(static_cast<uint16_t>(r.job_quest_id));
                p.write<uint16_t>(static_cast<uint16_t>(index));
                send(p);
                return;
            }
        }
    }
    PacketBuffer c(static_cast<Opcode>(0x01EB), is_paula());          // S2C 491 [u16 master] (the client
    c.write<uint16_t>(static_cast<uint16_t>(n.object_id));             // reads nothing: the window closes)
    send(c);
}

// C2S 489 (state 25).
void GameClientSession::handle_skill_shop_next(PacketBuffer& pkt)
{
    try {
        const uint16_t id = pkt.read<uint16_t>();
        const uint8_t index = pkt.read<uint8_t>();
        if (!sshop_) return;
        NpcEntity* n = server_.zone_skill_shop(zone_id_, id);
        if (!n || !n->sshop) {
            PacketBuffer c(static_cast<Opcode>(0x01EB), is_paula());  // S2C 491 and state 0
            send(c);
            skill_shop_leave();
            return;
        }
        send_skill_shop_msg(*n, index);
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 489 skill master message aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 489 skill master message aborted: unknown exception", session_id());
    }
}

// C2S 496 / 497 (state 25).
void GameClientSession::handle_skill_shop_script(PacketBuffer& pkt, bool params)
{
    try {
        const uint16_t id = pkt.read<uint16_t>();
        const uint8_t func = pkt.read<uint8_t>();
        if (!sshop_) return;
        NpcEntity* n = server_.zone_skill_shop(zone_id_, id);
        if (!n || !n->sshop) {
            PacketBuffer c(static_cast<Opcode>(0x01EB), is_paula());
            send(c);
            skill_shop_leave();
            return;
        }
        const size_t left = pkt.size() > pkt.rpos() ? pkt.size() - pkt.rpos() : 0;
        const int r = skill_shop_script(*n, func, pkt.data() + pkt.rpos(), left, params);
        if (r >= 1) {
            send_skill_shop_msg(*n, r);
        } else if (r == -1) {
            PacketBuffer c(static_cast<Opcode>(0x01EB), is_paula());  // S2C 491
            send(c);
        }
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 496/497 skill master script aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 496/497 skill master script aborted: unknown exception", session_id());
    }
}

// ExecuteSkillShopNpcScript (sub_7EA770) / ExecuteSkillShopNpcParamScript (sub_7EAB90).
int GameClientSession::skill_shop_script(NpcEntity& n, int func, const uint8_t* data, size_t size, bool params)
{
    LOG_DEBUG("%s:ExecuteSkillShopNpc%sScript: npcType:%d, funcId:%d", char_name_, params ? "Param" : "", n.sshop->type,
              func);
    SArg a{ data, size, 0 };
    int32_t pint[2] = { 0, 0 };                          // the first two parameters' numbers (sub_7ED080)
    if (params) {                                        // the parameters: [u8 n]{[u8 kind] u32 | cstr}
        uint8_t np = 0;
        a.u8(np);
        for (int i = 0; i < np; ++i) {
            uint8_t kind = 0;
            if (!a.u8(kind)) break;
            if (kind == 0) {
                uint32_t v = 0;
                a.u32(v);
                if (i < 2) pint[i] = static_cast<int32_t>(v);
            } else if (kind == 1) {
                a.cstr();
            }
        }
    }
    uint8_t cnt = 0;
    a.u8(cnt);
    const SkillShopRow& row = *n.sshop;
    const uint8_t* args = data + a.pos;                  // the u16 arguments
    const size_t args_size = size - a.pos;
    int r = -1;
    if (!params) {
        switch (func) {
        case 25:                                         // SkillQuest_Accept (sub_7E3970)
            r = skill_quest_accept(args, args_size, cnt);
            break;
        case 26:                                         // SkillQuest_Complete (sub_7E3A70): every path ends
            r = -1;                                      // -1 — its request (sub_7EC160) is "Deprecated"
            break;
        case 61:                                         // CJQuest_Check
        case 111:                                        // CJQuest2_Check
            r = cj_quest_check(n, func == 111);
            break;
        case 62:                                         // CJQuest_Accept
        case 112:                                        // CJQuest2_Accept
            r = cj_quest_accept(n, args, args_size, cnt);
            break;
        case 63:                                         // CJQuest_Complete
        case 113:                                        // CJQuest2_Complete
            r = cj_quest_complete(n, args, args_size, cnt);
            break;
        case 1:                                          // Exit
        case 38: {                                       // SkillShop_Exit: S2C 492 (__fputchar_70), state 0
            PacketBuffer p(static_cast<Opcode>(0x01EC), is_paula());
            send(p);
            skill_shop_leave();
            r = 0;
            break;
        }
        case 36:                                         // SkillShop_Buy
            send_skill_shop_buy_list(n);
            r = 0;
            break;
        case 37:                                         // SkillShop_Sell
            send_skill_shop_sell_list(n);
            r = 0;
            break;
        case 39:                                         // SkillMaster_Check (sub_7EA0F0: nothing)
            r = 0;
            break;
        case 59:                                         // SkillMasterUI
            send_skill_master_list(n);
            r = 0;
            break;
        case 60: {                                       // MagicElementUI: S2C 559 (__fputchar_86)
            PacketBuffer p(static_cast<Opcode>(0x022F), is_paula());
            send(p);
            r = 0;
            break;
        }
        case 64: {                                       // CJUIOpen: S2C 568 (sub_5FE870)
            PacketBuffer p(static_cast<Opcode>(0x0238), is_paula());
            p.write<uint16_t>(static_cast<uint16_t>(row.change_type));
            p.write<uint16_t>(static_cast<uint16_t>(row.change_job));
            send(p);
            r = 0;
            break;
        }
        default:                                         // (24 SkillQuest_Check is gone from the switch)
            break;
        }
    } else {
        switch (func) {
        case 1:                                          // Exit (sub_7E0850): S2C 117, state 0
            exit_npc_dialog();
            skill_shop_leave();
            r = 0;
            break;
        case 24: {                                       // SkillQuest_Check (sub_7E3870) [qid][skill]:
            const LearnedSkill* s = find_skill(pint[1]); // the skill, learned and still at level 0
            if (!s || s->level > 0 || pint[0] <= 0) break;
            skq_check_qid_   = static_cast<int16_t>(pint[0]);   // Player +2488 / +2492
            skq_check_skill_ = pint[1];
            if (!qinv_find(skquests_, pint[0], kSkillQuestCap)) { r = 1; break; }
            const QuestDef* d = server_.quest_db().get(pint[0]);
            if (d) r = quest_items_ok(*d, false) ? 2 : 3;
            break;
        }
        case 39:                                         // (sub_7EA100: nothing)
            r = 0;
            break;
        default:
            break;
        }
    }
    if (r < 1) return r;
    if (r > cnt) {
        LOG_ERROR("%s Invalid SkillShopnpcScript arg cnt:funcId:%d, res:%d, cntArg:%d, npctype:%d, zoneNo:%d", char_name_,
                  func, r, cnt, row.type, zone_id_);
        return -1;
    }
    uint16_t v = 0;
    for (int i = 0; i < r; ++i) {
        if (!a.u16(v)) {
            LOG_ERROR("%s Invalid SkillShopnpcScript res:funcId:%d, res:%d, cntArg:%d", char_name_, func, r, cnt);
            return -1;
        }
    }
    return v;
}

// S2C 500 (sub_5FD870).
void GameClientSession::send_skill_shop_buy_list(const NpcEntity& n)
{
    const ShopRow& s = n.sshop->shop;
    PacketBuffer p(static_cast<Opcode>(0x01F4), is_paula());
    uint16_t cnt = 0;
    for (const ShopItemRow& r : s.items) if (!r.out_of_stock) ++cnt;
    p.write<uint16_t>(cnt);
    for (const ShopItemRow& r : s.items) {
        if (r.out_of_stock) continue;
        p.write<uint32_t>(r.item_id);
        p.write<uint32_t>(static_cast<uint32_t>(shop_buy_price(s, r.item_id, false)));
    }
    send(p);
}

// S2C 501 (sub_5FD970).
void GameClientSession::send_skill_shop_sell_list(const NpcEntity& n)
{
    const ShopRow& s = n.sshop->shop;
    const MapDef* z = server_.maps_db().get(zone_id_);
    PacketBuffer p(static_cast<Opcode>(0x01F5), is_paula());
    p.write<float>((z ? z->shop_price_rate : 1.0f) * s.selling_rate);
    uint16_t cnt = 0;
    for (const ShopItemRow& r : s.items) if (r.selling_rate > 0.0f) ++cnt;
    p.write<uint16_t>(cnt);
    for (const ShopItemRow& r : s.items) {
        if (r.selling_rate <= 0.0f) continue;
        p.write<uint32_t>(r.item_id);
        p.write<float>(r.selling_rate);
    }
    send(p);
}

// S2C 557 (sub_5FE5A0): our skills at level 10 that have a SkillMasterInfo row and that the master masters.
void GameClientSession::send_skill_master_list(const NpcEntity& n)
{
    PacketBuffer p(static_cast<Opcode>(0x022D), is_paula());
    std::vector<uint16_t> ids;
    for (const auto& s : skills_)
        if (s.level == 10 && server_.skill_shop_db().master(s.id) && n.sshop && n.sshop->masters(s.id))
            ids.push_back(s.id);
    p.write<uint16_t>(static_cast<uint16_t>(ids.size()));
    for (uint16_t id : ids) p.write<uint16_t>(id);
    send(p);
}

// C2S 503 / 505 (state 25).
void GameClientSession::handle_skill_shop_list(PacketBuffer& pkt, bool sell)
{
    try {
        const uint16_t id = pkt.read<uint16_t>();
        if (!sshop_) return;
        NpcEntity* n = server_.zone_skill_shop(zone_id_, id);
        if (!n || !n->sshop) {
            // retail: S2C 504 / 506 (__fputchar_71 / _72) — the KR client has no handler for them
            LOG_INFO("GameClientSession %u: skill shop list — master %u not found", session_id(), id);
            return;
        }
        if (sell) send_skill_shop_sell_list(*n);
        else      send_skill_shop_buy_list(*n);
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 503/505 skill card list aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 503/505 skill card list aborted: unknown exception", session_id());
    }
}

// C2S 492 / 502 (state 25): state 0.
void GameClientSession::handle_skill_shop_close()
{
    skill_shop_leave();
}

// ── The change-job quests (the skill quests, Player +2472) ────────────────────────────────────────────

// The login: uspm_char_all_select_char_info_improve's skill quests (char_quest type 3) come as their ids
// alone (sub_8ACB80: at most 30, else "create client : invalid skill quest item cnt"), so the login info's
// run stays 0 (its memset) and SetPlayerFromLoginInfo adds each with run 0 to the one-entry QuestInv (a
// second one finds no free entry). The same on a zone's re-enter.
void GameClientSession::load_skill_quests()
{
    for (QuestEntry& e : skquests_) e = QuestEntry{};
    if (!char_uid_) return;
    const DBResult rows = server_.db().query("SELECT quest_id FROM char_quest WHERE char_uid=%u AND quest_type=%d",
                                             char_uid_, kSqlSkillQuest);
    if (rows.size() > 30)
        LOG_ERROR("%s : create client : invalid skill quest item cnt %zu", char_name_, rows.size());
    size_t n = 0;
    for (const DBRow& r : rows) {
        if (n++ >= 30) break;
        auto q = r.find("quest_id");
        if (q != r.end()) qinv_add(skquests_, std::atoi(q->second.c_str()), 0, false, kSkillQuestCap);
    }
}

// S2C 296 (sub_5F6CB0, the login sequence): [u16 n]{u32 qid} — the login info's list.
void GameClientSession::send_skill_quest_list()
{
    std::vector<int32_t> ids;
    if (char_uid_) {
        const DBResult rows = server_.db().query("SELECT quest_id FROM char_quest WHERE char_uid=%u AND quest_type=%d",
                                                 char_uid_, kSqlSkillQuest);
        for (const DBRow& r : rows) {
            if (ids.size() >= 30) break;
            auto q = r.find("quest_id");
            if (q != r.end()) ids.push_back(std::atoi(q->second.c_str()));
        }
    }
    PacketBuffer p(static_cast<Opcode>(0x0128), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(ids.size()));
    for (int32_t id : ids) p.write<uint32_t>(static_cast<uint32_t>(id));
    send(p);
}

// CJQuest_Check (61, second = 0) / CJQuest2_Check (111, second = 1), sub_7EA110: 6 another job's master
// (LimitJobType) or another change's quest done; 4 this change's quest done; 6 the wrong tier (the base
// jobs / the 2nd-tier ones); 5 below level 60 / TM 50 (the 2nd change: 130 / 120); 7 without the pact
// the change asks for; the quest in progress -> 2 with its items, else 3; else 1. Player +2488 = the quest.
int GameClientSession::cj_quest_check(const NpcEntity& n, bool second)
{
    const SkillShopRow& row = *n.sshop;
    if (row.limit_job_type && row.limit_job_type != job_) return 6;
    if (cj_state_) return cj_state_ == row.change_job ? 4 : 6;
    if (second) {
        if (job_ < 10 || job_ > 18) return 6;            // (the EXE: < 9 || > 16)
    } else if (job_ > 9) {                               // (the EXE: > 8)
        return 6;
    }
    if (row.job_quest_id <= 0) return -1;
    if (second ? (level_ < 130 || tm_level_ < 120) : (level_ < 60 || tm_level_ < 50)) return 5;
    skq_check_qid_ = row.job_quest_id;
    if (const int pact = pact_skill(row.change_job)) if (!find_skill(pact)) return 7;
    if (!qinv_find(skquests_, row.job_quest_id, kSkillQuestCap)) return 1;
    const QuestDef* d = server_.quest_db().get(row.job_quest_id);
    if (!d) return -1;
    return quest_items_ok(*d, false) ? 2 : 3;            // sub_6DB140: all of SrcItem
}

// CJQuest_Accept (62 / 112), sub_7EA340: -1 no QuestInfo or taken already, 2 the entry is in use, 3 without
// the pact; the 1st argument (its message) or -1; hItemAcceptQuest type 7 (sub_7ED110 -> the Master's
// uspm_quest_take, SQL type 3: run 1) -> OnOk (sub_77C490 -> sub_77CA60): the entry (run 1) and S2C 294
// [u32 QuestInfo Id][u16 run][cstr Name][cstr Client][cstr Pos][cstr Comment] (sub_5FA290), then the
// message (sub_5FD4D0 -> S2C 490); no entry -> uspm_quest_cancel and S2C 491. A DB failure: OnCancel
// (sub_77C900) S2C 116.
int GameClientSession::cj_quest_accept(NpcEntity& n, const uint8_t* args, size_t size, int cnt)
{
    const SkillShopRow& row = *n.sshop;
    const QuestDef* d = server_.quest_db().get(row.job_quest_id);
    if (!d) return -1;
    if (qinv_find(skquests_, d->id, kSkillQuestCap)) return -1;
    bool free_entry = false;
    for (const QuestEntry& e : skquests_) if (!e.used) { free_entry = true; break; }
    if (!free_entry) return 2;                           // sub_7DBE80
    if (const int pact = pact_skill(row.change_job)) if (!find_skill(pact)) return 3;
    int msg = -1;
    if (!sshop_arg(args, size, cnt, 1, msg)) return -1;
    int com = 0;
    const int err = db_quest_take(kSqlSkillQuest, row.job_quest_id, 0, 0, com);
    if (err > 0) {
        LOG_ERROR("QuestStore::AcceptQuest SP failure : spRet:%d, chr:%u, qType:%d, qid:%d, maxCnt:%d, Count:%d", err,
                  char_uid_, kSqlSkillQuest, row.job_quest_id, 0, com);
        send_npc_cancel();
        return -2;
    }
    QuestEntry* e = qinv_add(skquests_, row.job_quest_id, 1, false, kSkillQuestCap);   // sub_7DBFC0(qid, 1)
    if (!e) {
        db_quest_cancel(row.job_quest_id, 0);            // sub_77C060
        LOG_ERROR("-- hItemAcceptQuest::OnOk - but !bRet : name(%s), qid(%d), qtype(%d)", char_name_,
                  row.job_quest_id, 7);
        PacketBuffer c(static_cast<Opcode>(0x01EB), is_paula());      // S2C 491 (__fputchar_69)
        send(c);
        return -2;
    }
    if (e->def) {
        PacketBuffer p(static_cast<Opcode>(0x0126), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(e->def->id));
        p.write<uint16_t>(static_cast<uint16_t>(e->count));
        for (const std::string* s : { &e->def->name, &e->def->client, &e->def->pos, &e->def->comment }) {
            const std::string t = to_client_text(*s);
            p.write_bytes(t.c_str(), t.size() + 1);
        }
        send(p);
    }
    LOG_INFO("GameClientSession %u: change-job quest %d taken (master %u)", session_id(), row.job_quest_id,
             n.object_id);
    send_skill_shop_msg(n, msg);
    return -2;
}

// CJQuest_Complete (63 / 113), sub_7EA4B0: -1 no QuestInfo, not in progress or its items missing; 5 without
// the pact; -1 a single (non-common) item it asks for is equipped (sub_7606E0); the run's result
// (sub_6D8680; Type 1: one of its items at random); the weight after (- SrcItem + the rewards) above
// the max or fewer free entries than reward lines -> 2; the 1st argument (the run under MaxCount) or the
// 3rd. hItemCompleteQuest type 7 (sub_7ED220 -> sub_7EB3B0: only when the player's NPC is a master whose
// ChangeJob is set — it goes as the quest state): the Master's uspm_quest_completed (SQL type 3: no
// history) and uspm_char_update_change_job_queststate (refused once a 3rd job is there) -> OnOk
// (sub_77CC60 -> sub_77EB80): the bag records, QuestInfo's EXP / TM EXP (sub_77E220), S2C 150 [records]
// [the rewards][u32 EXP][u32 TM EXP] and zeros (qid, run, gelder, skill), the state = ChangeJob, the
// message (S2C 490), then the entry goes and S2C 295 [u32 qid]. A DB failure: OnCancel (sub_77D640) S2C 116.
int GameClientSession::cj_quest_complete(NpcEntity& n, const uint8_t* args, size_t size, int cnt)
{
    const SkillShopRow& row = *n.sshop;
    const QuestDef* d = server_.quest_db().get(row.job_quest_id);
    if (!d) return -1;
    QuestEntry* e = qinv_find(skquests_, row.job_quest_id, kSkillQuestCap);
    if (!e) return -1;
    if (!quest_items_ok(*d, false)) return -1;
    if (const int pact = pact_skill(row.change_job)) if (!find_skill(pact)) return 5;
    for (const QuestItem& it : d->src) {                 // sub_7606E0
        const ItemDef* idf = server_.item_db().get(it.item);
        if (!it.item || !idf || idf->stackable()) continue;
        for (uint64_t uid : equip_) {
            const BagItem* b = uid ? bag_find_uid(uid) : nullptr;
            if (b && b->item_id == it.item) return -1;
        }
    }
    const QuestResult* res = d->result_for(e->count);
    if (!res) return -1;
    std::vector<QuestItem> rewards;
    if (res->type) {
        if (res->items.empty()) return -1;               // (rand() % 0 in the retail)
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
    if (!sshop_arg(args, size, cnt, e->count < d->max_count ? 1 : 3, msg)) return -1;
    if (!row.change_job) {
        // sub_7EB3B0 sends nothing for a master without a ChangeJob (and the retail's request flag stays set).
        LOG_ERROR("%s : CJQuest_Complete at master %u without a ChangeJob — no request", char_name_, n.object_id);
        return -2;
    }
    const int err = db_quest_completed(kSqlSkillQuest, row.job_quest_id, 0, 0, 0, true);
    if (err) {
        LOG_ERROR("QuestStore::CompletedQuest SP failure : spRet:%d, chr:%u, qType:%d, qid:%d", err, char_uid_,
                  kSqlSkillQuest, row.job_quest_id);
        send_npc_cancel();
        return -2;
    }
    // uspm_char_update_change_job_queststate: refused (101) when a 3rd job is there — "UpdateCJQuestState:
    // failed", the quest completes all the same.
    if (!job3_)
        server_.db().execute("UPDATE characters SET change_job_queststate=%d WHERE uid=%u AND job_3=0", row.change_job,
                             char_uid_);
    else
        LOG_ERROR("UpdateCJQuestState: failed: %u", char_uid_);
    const std::vector<BagRecord> applied = quest_bag_records(d->src, rewards);
    pet_speech(15);                                      // sub_77ED80 (every OnOk): our pet's line
    const int32_t exp  = d->exp;
    const int32_t tmxp = d->tmxp;
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
        PacketBuffer p(static_cast<Opcode>(0x0096), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(applied.size()));
        for (const BagRecord& r : applied) write_bag_record(p, r);
        p.write<uint16_t>(static_cast<uint16_t>(rewards.size()));
        for (const QuestItem& it : rewards) {
            p.write<uint32_t>(it.item);
            p.write<uint16_t>(static_cast<uint16_t>(it.cnt));
        }
        p.write<uint32_t>(static_cast<uint32_t>(exp));
        p.write<uint32_t>(static_cast<uint32_t>(tmxp));
        p.write<uint16_t>(0);
        p.write<uint16_t>(0);
        p.write<uint32_t>(0);
        p.write<uint16_t>(0);
        p.write<uint16_t>(0);
        send(p);
    }
    cj_state_ = static_cast<uint8_t>(row.change_job);    // sub_40E850 (char info +209)
    save_position();                                     // the EXP
    LOG_INFO("GameClientSession %u: change-job quest %d completed (run %d): %zu records, EXP %d, TM %d -> job %d",
             session_id(), row.job_quest_id, e->count, applied.size(), exp, tmxp, row.change_job);
    send_skill_shop_msg(n, msg);
    qinv_remove(skquests_, row.job_quest_id, kSkillQuestCap);
    PacketBuffer rm(static_cast<Opcode>(0x0127), is_paula());          // S2C 295 (sub_5FA370)
    rm.write<uint32_t>(static_cast<uint32_t>(d->id));
    send(rm);
    return -2;
}

// SkillQuest_Accept (25), sub_7E3970, with the quest of Player +2488: -1 no QuestInfo or taken, 2 the entry
// in use; the 1st argument. Its request is the GameServer quest type 3 (sub_7EB0E0), which the Master
// takes as SQL type 0 (dword_591AA4[3]: no char_quest row) — and the function answers -1 at once (S2C
// 491); the OnOk (sub_77C490) has no type-3 branch: "!bRet", S2C 116. No dialog of the data calls it.
int GameClientSession::skill_quest_accept(const uint8_t* args, size_t size, int cnt)
{
    if (!server_.quest_db().get(skq_check_qid_)) return -1;
    if (qinv_find(skquests_, skq_check_qid_, kSkillQuestCap)) return -1;
    bool free_entry = false;
    for (const QuestEntry& e : skquests_) if (!e.used) { free_entry = true; break; }
    if (!free_entry) return 2;
    int msg = -1;
    if (!sshop_arg(args, size, cnt, 1, msg)) return -1;
    PacketBuffer c(static_cast<Opcode>(0x01EB), is_paula());          // the -1: S2C 491
    send(c);
    LOG_ERROR("-- hItemAcceptQuest::OnOk - but !bRet : name(%s), qid(%d), qtype(%d)", char_name_, skq_check_qid_, 3);
    send_npc_cancel();                                   // S2C 116
    return -2;
}

// ── The job change ─────────────────────────────────────────────────────────────────────────────────

// C2S 569 [u16 master] (the state 25's case 569, client CJ window): no such master -> S2C 491 and state 0;
// a change to Dark Lord / Priest without its pact -> the same; another job's master (LimitJobType) or
// another change's quest done (the state set and not this ChangeJob) -> S2C 571 (the state stays); the
// equipped items need free bag entries (sub_760F80) else S2C 571 and state 0; then hItemChangeJob.
// (The retail checks neither the level nor that this change's quest was done — a state of 0 passes;
// ours adds both, see the Hardening note below.)
void GameClientSession::handle_change_job(PacketBuffer& pkt)
{
    try {
        const uint16_t id = pkt.read<uint16_t>();
        if (!sshop_) return;                             // (state 25 only)
        auto fail = [&](uint16_t op, bool leave) {
            PacketBuffer p(static_cast<Opcode>(op), is_paula());
            send(p);
            if (leave) skill_shop_leave();
        };
        NpcEntity* n = server_.zone_skill_shop(zone_id_, id);
        if (!n || !n->sshop) { fail(0x01EB, true); return; }
        const SkillShopRow& row = *n->sshop;
        if (const int pact = pact_skill(row.change_job)) {
            if (!find_skill(pact)) { fail(0x01EB, true); return; }
        }
        if (row.limit_job_type && row.limit_job_type != job_) { fail(0x023B, false); return; }
        if (cj_state_ && cj_state_ != row.change_job) { fail(0x023B, false); return; }
        // Hardening (not retail): retail 569 checks neither this change's quest nor the level (only the dialog's
        // CJQuest_Check sub_7EA110 does), so a forged 569 took the 2nd / 3rd job at level 1. A real client gets
        // here only with the quest done (state = ChangeJob) and level 60 / TM 50 (2nd job) or 130 / 120 (3rd).
        // The one row without a JobQuestID (SkillShrine21 Type 6313, ChangeJob 20) has no change dialog — none of
        // its messages has CJQuest or CJUIOpen — so no real client sends 569 for it: refused like a missing quest.
        {
            const bool third    = job_tier(row.change_job) == 3;
            const bool quest_ok = row.job_quest_id > 0 && cj_state_ == row.change_job;
            const bool level_ok = third ? (level_ >= 130 && tm_level_ >= 120) : (level_ >= 60 && tm_level_ >= 50);
            if (!quest_ok || !level_ok) {
                LOG_INFO("GameClientSession %u: job change to %d refused (quest state %d, level %d, TM %d)",
                         session_id(), row.change_job, cj_state_, level_, tm_level_);
                fail(0x023B, true);
                return;
            }
        }
        int equipped = 0;
        for (uint64_t uid : equip_) if (uid) ++equipped;
        if (equipped > 300 - static_cast<int>(bag_.size())) { fail(0x023B, true); return; }
        change_job(*n);
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 569 job change aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 569 job change aborted: unknown exception", session_id());
    }
}

// hItemChangeJob (sub_78A0A0): a class out of range (the EXE: >= 34 — none) -> S2C 571 twice and state 0;
// the tier of ChangeJob; the Master's OnCCmdChangeOccupation (tiers 2 / 3 only, else 3662) ->
// uspm_char_attr_update_get_change_job: type_N / job_N / job_N_changed, the quest state 0, hair_N = 0 (the
// GameServer sends hair 0), job_state = N, and every equipped item back to the bag. OnCancel (sub_78A6B0):
// S2C 571, S2C 492, state 0. OnOk (sub_78A210): the char info (tier N's type / job, its hair 0, job_state,
// quest state 0), Player::SetJobState (sub_761250; a model id >= 25 for the 1st / 2nd tier -> "SetMakeUp()
// Error", S2C 492), every equipped item comes off (sub_7613E0: the rare ones slot by slot — S2C 85 to us,
// S2C 237 to the area — then the throw slot and the magazine — S2C 521 [uid][20]), S2C 570 to the area
// [u16 user][u16 model +436][u8 class +1024][u8 hair +440][u8 job_state +4680][u8 0] (sub_5FE8F0; client
// avatar / remote sub_6CC820 + "ChangeJob_Complete"), S2C 492, S2C 582 to the area [u16 user][the equipment
// masks] (sub_5FEC70), state 0. Not modelled: the community server's 5535 (sub_5FEBC0), the game log
// (sub_78A4A0), and a 3rd job's world notice — ChangeJobStrings CJNotice through the Master's system
// megaphone (3699 -> uspm_event_insert_broadcast_message), logged here.
void GameClientSession::change_job(NpcEntity& n)
{
    const SkillShopRow& row = *n.sshop;
    auto cancel = [&](bool twice) {
        PacketBuffer c(static_cast<Opcode>(0x023B), is_paula());       // S2C 571 (__fputchar_90)
        send(c);
        if (twice) send(c);
        else {
            PacketBuffer x(static_cast<Opcode>(0x01EC), is_paula());   // S2C 492
            send(x);
        }
        skill_shop_leave();
    };
    if (job_ >= 37) { cancel(true); return; }
    const int tier = job_tier(row.change_job);
    if (tier != 2 && tier != 3) {
        LOG_ERROR("OnCCmdChangeOccupation: Unknown jobIndex:%d", tier);
        cancel(false);
        return;
    }
    // uspm_char_attr_update_get_change_job
    const bool ok = server_.db().execute(
        tier == 2 ? "UPDATE characters SET type_2=%d, job_2=%d, job_2_changed=NOW(), change_job_queststate=0, hair_2=0, "
                    "job_state=2 WHERE uid=%u"
                  : "UPDATE characters SET type_3=%d, job_3=%d, job_3_changed=NOW(), change_job_queststate=0, hair_3=0, "
                    "job_state=3 WHERE uid=%u",
        row.change_type, row.change_job, char_uid_) > 0;
    if (!ok) {
        LOG_ERROR("uspm_char_attr_update_get_change_job (%u, %d, %d, 0, %d) : 101", char_uid_, row.change_type,
                  row.change_job, tier);
        cancel(false);
        return;
    }
    server_.db().execute("UPDATE char_inventory SET equip_slot=-1 WHERE char_uid=%u AND item_where=1 AND equip_slot>=0",
                         char_uid_);
    // OnOk: the char info, then SetJobState.
    if (tier == 2) { type2_ = static_cast<int16_t>(row.change_type); job2_ = static_cast<uint8_t>(row.change_job); hair2_ = 0; }
    else           { type3_ = static_cast<int16_t>(row.change_type); job3_ = static_cast<uint8_t>(row.change_job); hair3_ = 0; }
    job_state_ = static_cast<uint8_t>(tier);
    cj_state_  = 0;
    if (base_type_ >= 25 || type2_ >= 25) {
        LOG_ERROR("%s : hItemChangeJob::OnOk::SetMakeUp() Error", char_name_);
        PacketBuffer x(static_cast<Opcode>(0x01EC), is_paula());
        send(x);
        skill_shop_leave();
        return;
    }
    apply_job_state();
    // sub_7613E0: the equipped items come off.
    std::vector<uint64_t> release;
    for (int i = 0; i < 23; ++i) {
        const BagItem* b = equip_[i] ? bag_find_uid(equip_[i]) : nullptr;
        if (b && b->kind == 2) release.push_back(equip_[i]);
    }
    if (throw_item_) release.push_back(throw_item_);
    if (mag_.item) release.push_back(mag_.item);
    equip_n_release(-1, release, 0);
    {
        PacketBuffer p(static_cast<Opcode>(0x023A), is_paula());       // S2C 570
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<uint16_t>(static_cast<uint16_t>(type_));
        p.write<uint8_t>(static_cast<uint8_t>(job_));
        p.write<uint8_t>(shown_hair());
        p.write<uint8_t>(job_state_);
        p.write<uint8_t>(0);
        server_.broadcast_zone(zone_id_, p, 0);
    }
    {
        PacketBuffer x(static_cast<Opcode>(0x01EC), is_paula());       // S2C 492
        send(x);
    }
    {
        PacketBuffer p(static_cast<Opcode>(0x0246), is_paula());       // S2C 582
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        write_equip_appearance(p);
        server_.broadcast_zone(zone_id_, p, 0);
    }
    if (job_ >= 19) {                                    // (the EXE: class >= 17, a 3rd job)
        // ChangeJobStrings CJNotice with NewSkillUIParams "JobTab_<class>" — the server's table has only JobTab_All,
        // and a missing key reads as itself (sub_8E2300) — through the system megaphone (sub_602CB0, "Trickster")
        char tab[32], msg[256];
        std::snprintf(tab, sizeof(tab), "JobTab_%d", job_ - 2);
        std::snprintf(msg, sizeof(msg), "%s님이 %s(으)로 전직하셨습니다. 모두 축하해 주세요.", char_name_, tab);
        server_.sys_megaphone("트릭스터", msg);
    }
    LOG_INFO("GameClientSession %u: job change at master %u -> tier %d, model %d, job %d (%zu items came off)",
             session_id(), n.object_id, tier, type_, job_, release.size());
    skill_shop_leave();
}

// ── Skill mastery ─────────────────────────────────────────────────────────────────────────────────

// C2S 560 [u16 master][u16 skill] (Player case 560, a message of every state; client SkillMasterUI): no
// such master or skill -> S2C 562 ("SkillMasterCancel"); the record {skill, 11, MasterTMPoint}; not
// CanLevelUpSkill(11) -> S2C 562 when the TM points cover MasterTMPoint, else S2C 557 (the list again)
// and S2C 564 ("TmNotEnough"); no SkillMasterInfo row, none of its item in the bag (sub_6DCC50) or fewer
// than ItemCnt of it (sub_6DADA0) -> S2C 562. hItemSkillMaster (sub_780EC0): the record taking ItemCnt of
// the first such item (sub_6E4460), the DB (4017, item record type 29) -> OnOk (sub_780FD0): TM points
// below the cost -> "NPC_SKILLMASTER_CANCEL" and nothing more; the TM points go, S2C 524 [the record] to
// us and S2C 482 [u16 user][u16 FType][u16 11] to the area (sub_5FD1E0), the skill's level 11, the DB
// (4504), the item record applied and sent — S2C 561 [record] (sub_5FE710; client avatar: the bag) —
// then S2C 557 from the master. (OnCancel sub_7811E0: a DB failure — none here.) All VERIFIED in the
// client (avatar sub_6B4E00 cases 561 / 562 / 564 / 524, sub_6CCBF0 case 482, sub_6F0960 case 557).
void GameClientSession::handle_skill_master(PacketBuffer& pkt)
{
    try {
        const uint16_t id = pkt.read<uint16_t>();
        auto fail = [&](uint16_t op) {
            PacketBuffer p(static_cast<Opcode>(op), is_paula());
            send(p);
        };
        NpcEntity* n = server_.zone_skill_shop(zone_id_, id);
        if (!n || !n->sshop) { fail(0x0232); return; }
        const uint16_t skill = pkt.read<uint16_t>();
        LearnedSkill* s = nullptr;
        for (LearnedSkill& x : skills_) if (x.id == skill) { s = &x; break; }
        const SkillDef* d = server_.skill_db().get(skill);
        if (!s || !d) { fail(0x0232); return; }
        const int cost = static_cast<int16_t>(d->master_tm);
        if (!can_level_up_skill(skill, 11)) {
            if (tm_points_ >= cost) {
                fail(0x0232);
            } else {
                send_skill_master_list(*n);
                fail(0x0234);
            }
            return;
        }
        const SkillMasterInfo* mi = server_.skill_shop_db().master(skill);
        BagItem* b = mi ? bag_find_id(mi->item) : nullptr;
        if (!mi || !b) { fail(0x0232); return; }
        int have = 0;
        for (const BagItem& x : bag_) {
            if (x.item_id != mi->item) continue;
            const ItemDef* idf = server_.item_db().get(x.item_id);
            have += idf ? idf->item_count(x.count) : x.count;
        }
        if (have < mi->count) { fail(0x0232); return; }
        // hItemSkillMaster: the item record, the DB.
        const BagRecord rec = bag_decrease(*b, static_cast<uint16_t>(mi->count));
        if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
        else               server_.item_store().SetAmount(rec.uid, rec.count);
        // OnOk
        if (tm_points_ < cost) {
            LOG_INFO("Player::NPC_SKILLMASTER_CANCEL %u,%s", static_cast<unsigned>(object_id_), char_name_);
            return;
        }
        tm_points_ -= cost;
        uint8_t r5[5];
        r5[0] = static_cast<uint8_t>(skill & 0xFF);
        r5[1] = static_cast<uint8_t>(skill >> 8);
        r5[2] = 11;
        r5[3] = static_cast<uint8_t>(cost & 0xFF);
        r5[4] = static_cast<uint8_t>((cost >> 8) & 0xFF);
        {
            PacketBuffer own(static_cast<Opcode>(0x020C), is_paula());  // S2C 524
            own.write_bytes(r5, sizeof(r5));
            send(own);
            PacketBuffer area(static_cast<Opcode>(0x01E2), is_paula()); // S2C 482
            area.write<uint16_t>(static_cast<uint16_t>(object_id_));
            area.write<uint16_t>(f_type_);
            area.write<uint16_t>(11);
            server_.broadcast_zone(zone_id_, area, 0);
        }
        s->level = 11;
        server_.db().execute("UPDATE char_skills SET level=11 WHERE char_uid=%u AND skill_id=%d", char_uid_, skill);
        bag_apply(rec);
        {
            PacketBuffer p(static_cast<Opcode>(0x0231), is_paula());   // S2C 561
            write_bag_record(p, rec);
            send(p);
        }
        save_stat_points();                              // the TM points
        LOG_INFO("GameClientSession %u: skill %d mastered at master %u (item %u x%d, TM points -%d -> %d)",
                 session_id(), skill, id, mi->item, mi->count, cost, tm_points_);
        send_skill_master_list(*n);
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 560 skill mastery aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 560 skill mastery aborted: unknown exception", session_id());
    }
}
