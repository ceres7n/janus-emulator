#include "game_session.hpp"
#include "game_server.hpp"
#include "system_session.hpp"
#include "client_text.hpp"
#include "logger.hpp"
#include <cstdlib>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  The Mic (Effect 33, e.g. item 19655) — 1:1 with the retail GameServer (Player::OnProcessState sub_73D2A0
//  case 800 @0x747E1B, read from its disassembly), the client's side verified in Trickster.bin:
//  - C2S 800 (client c2s_800, from the chat input sub_8BDC90: it looks for an Effect-33 item in the bag; its own
//    10 s delay, "msg_limited_by_megaphone_delay") [u64 item uid][str text].
//  - The item, in the bag (+1716) — none: the log "has no item"; not of Effect 33: "invalid item". The text. The
//    shell (Character +108, its +8 = 1 — "invalid shell or shell type"; not modelled: always one). Only in the
//    Player states 0 / 8 / 15 (normal, sitting, resting; any other: nothing). The Mic is not used up.
//  - S2C 800 [u16 us][str name][str text] to our area (sub_602930; client case 800: the chat line "[name] text",
//    its type 1 when the speaker is not in sight).
//  - The conditions 2 / 3 / 8 end (sub_73C9C0 / sub_73CAF0 / sub_73CCD0: S2C 183 / 318 / 816 [u16 us] to the area;
//    2 and 3 do not exist here), then condition 8 — the head notice — with the text (Player +3519, 161 chars,
//    sub_76E300) and S2C 815 [u16 us][str text] to the area (sub_602BA0; client case 815: the balloon over our
//    head, sub_6CDE70; appear strMask bit 0x10 carries it to newcomers — sub_73AB30, client sub_6CDF30).
//  The conditions end on the C2S cases 1, 22, 24, 29, 33, 44, 54, 315, 331, 417, 468, 477, 713, 791 and 800
//  (the switch's calls of sub_73CCD0) and on the server's own walks (sub_4218A0 / sub_76B660): head_notice_end.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
std::string read_cstr(PacketBuffer& pkt)
{
    std::string s;
    while (pkt.rpos() < pkt.size()) {
        const char ch = static_cast<char>(pkt.read<uint8_t>());
        if (!ch) break;
        s += ch;
    }
    return s;
}

// The first `max` bytes of UTF-8 text without cutting a character (a cut one is invalid UTF-8: a strict-mode DB
// refuses the row, ERROR 1366).
std::string utf8_cut(const std::string& s, size_t max)
{
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // back to the cut character's lead
    return s.substr(0, n);
}

// The first `max` bytes of CP949 text without cutting a double-byte character.
std::string cp949_cut(const std::string& s, size_t max)
{
    size_t i = 0;
    while (i < s.size()) {
        const size_t len = (static_cast<unsigned char>(s[i]) >= 0x81 && i + 1 < s.size()) ? 2 : 1;
        if (i + len > max) break;
        i += len;
    }
    return s.substr(0, i);
}
} // namespace

// The Player FSM state (unknown_libname_146) as far as it is modelled: 3 drilling, 5 a shop, 6 a trade, 8 sitting,
// 9 an NPC talk, 10 the warehouse, 11 the bank, 13 a compound, 15 resting, 18 an exchange shop, 25 a skill master,
// 30..32 the refine UIs; 0 otherwise.
int GameClientSession::fsm_state() const
{
    if (drill_.active)  return 3;
    if (shop_)          return 5;
    if (trade_)         return 6;
    if (talk_npc_)      return 9;
    if (wh_state_)      return 10;
    if (bank_state_)    return 11;
    if (compound_)      return 13;
    if (exchange_)      return 18;
    if (sshop_)         return 25;
    if (refine_ui_)     return refine_ui_;
    return rest_state_;                                    // 8 / 15 / 0
}

// sub_73C9C0 / sub_73CAF0 / sub_73CCD0: the conditions 2 / 3 / 8 end — here only 8, the Mic's head notice: off, S2C 816
// [u16 us] to the area (sub_602C30; client case 816: the balloon goes, sub_6C71A0).
void GameClientSession::head_notice_end()
{
    if (!mega_on_) return;
    mega_on_ = false;
    mega_text_.clear();
    if (!object_id_) return;
    PacketBuffer p(static_cast<Opcode>(0x0330), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(object_id_));
    server_.broadcast_zone(zone_id_, p, 0);
}

void GameClientSession::handle_megaphone(PacketBuffer& pkt)
{
    if (state_ != GameClientSessionState::InWorld || leaving_ || !object_id_) return;
    uint64_t uid = 0;
    std::string text;
    try {
        uid  = pkt.read<uint64_t>();
    } catch (...) { return; }
    BagItem* b = bag_find_uid(uid);
    if (!b) {
        LOG_INFO("Player::OnProcessState - CMD_MEGAPHONE : %s has no item, itemUID : %llu", char_name_,
                 static_cast<unsigned long long>(uid));
        return;
    }
    const ItemDef* it = server_.item_db().get(b->item_id);
    if (!it || it->effect != 33) {
        LOG_INFO("Player::OnProcessState - CMD_MEGAPHONE : %s invalid item, itemUID : %llu", char_name_,
                 static_cast<unsigned long long>(uid));
        return;
    }
    try { text = read_cstr(pkt); } catch (...) { return; }
    const int st = fsm_state();
    if (st != 0 && st != 8 && st != 15) return;

    const uint16_t self = static_cast<uint16_t>(object_id_);
    {
        PacketBuffer p(static_cast<Opcode>(0x0320), is_paula());           // S2C 800
        p.write<uint16_t>(self);
        p.write_bytes(reinterpret_cast<const uint8_t*>(char_name_), std::strlen(char_name_) + 1);
        p.write_bytes(reinterpret_cast<const uint8_t*>(text.c_str()), text.size() + 1);
        server_.broadcast_zone(zone_id_, p, 0);
    }
    head_notice_end();
    mega_on_   = true;
    mega_text_ = text.substr(0, 161);
    {
        PacketBuffer p(static_cast<Opcode>(0x032F), is_paula());           // S2C 815
        p.write<uint16_t>(self);
        p.write_bytes(reinterpret_cast<const uint8_t*>(mega_text_.c_str()), mega_text_.size() + 1);
        server_.broadcast_zone(zone_id_, p, 0);
    }
    LOG_DEBUG("GameClientSession %u: Mic (item %u): %s", session_id(), b->item_id, mega_text_.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
//  The TBN Megaphone (Effect 32, items 19656 / 19667 / 19782; UseItemHelper::Use case 0x20 + hItemMegaphoneBooster
//  sub_6E50A0; the Master's OnCCmdMegaphone sub_40A270 / ProcessMegaphoneMessage sub_41B020; the SystemServer's
//  OnMegaphoneMessage sub_57D610), the client verified:
//  - The client (uMegaphoneBoosterView, sub_97F950: a Mic in the bag, else "msg_have_not_item") sends the custom
//    data [u64 the Mic's uid][str "[%s-%s] " — the island's name and ours][str text], then uses the booster.
//  - The Mic in the bag (+1716), of Effect 33 — else the log and S2C 803 [u32 1] (sub_602A50); the name and the
//    text present, the text at most 161 chars — else the log; then (sub_6E50A0) a request already running
//    (+1636 flag 0x80000000) -> nothing; else one of the booster held back (sub_6E4460), CCMD 3693 to the Master
//    with the message name + text and the flag set; the use answers 2 (the booster is used up on the answer).
//  - The Master (uspm_insert_broadcast_message: the message queued, flag 1 / type 1, the booster's amount - 1 or
//    the item gone) -> 3694 [the messages waiting before it] -> OnOk (sub_789690): the booster's record, S2C 802
//    [bag record][u32 waiting] (client case 802: the bag, "msg_megaphone_ack_ok"), the flag off; an error -> 3695
//    [code] -> OnCancel (sub_789760): S2C 803 [u32 code] (client "msg_megaphone_ack_cancel"), the flag off.
//  - Every MegaphoneDelay ms (MasterServer.ini 18000) the next queued message goes to each island's SystemServer
//    (5543), which sends S2C 801 [u16 broadcast_type][str message] to every community connection (client case
//    801: the chat line, colour 6, and the banner — type 1 splits "[island-name]text") and acks (5544); all
//    acked -> uspm_update_broadcast_success (flag 2 + the log).
//  Here the Master's part is ours (one island): the queue in cGameServer, the answer at once (no pending flag can
//  be left set), and the booster used up through the bag (the procedure's one transaction for the message and
//  the item becomes two steps).
// ─────────────────────────────────────────────────────────────────────────────
int GameClientSession::use_megaphone_booster(BagItem& b, const ItemDef& d)
{
    auto cancel = [&](uint32_t code) {                                   // S2C 803 (sub_602A50)
        PacketBuffer p(static_cast<Opcode>(0x0323), is_paula());
        p.write<uint32_t>(code);
        send(p);
    };
    const uint64_t mic_uid = use_data_.u64();
    std::string name, text;
    const bool has_name = use_data_.str(name);
    const bool has_text = use_data_.str(text);
    BagItem* mic = bag_find_uid(mic_uid);
    const ItemDef* md = mic ? server_.item_db().get(mic->item_id) : nullptr;
    if (!mic || !md) {
        LOG_INFO("%s: Microphone is not found.", char_name_);
        cancel(1);
        return 0;
    }
    if (md->effect != 33) {
        LOG_INFO("%s: item is not microphone type! (m_Effect:%d)", char_name_, md->effect);
        cancel(1);
        return 0;
    }
    if (!has_name) { LOG_INFO("%s: Megaphone Message name is NULL", char_name_); return 0; }
    if (!has_text) { LOG_INFO("%s: Megaphone Message text is NULL", char_name_); return 0; }
    if (text.size() > 161) {
        LOG_INFO("%s: Message buffer was exceeded: Size(%zu)", char_name_, text.size());
        return 0;
    }
    int32_t wait = 0;
    const int err = server_.megaphone_submit(char_uid_, from_client_text(name + text), wait);
    if (err) {                                                           // OnCancel (sub_789760)
        cancel(static_cast<uint32_t>(err));
        LOG_INFO("%s: %s", char_name_, "hItemMegaphoneBooster::OnCancel");
        return 2;
    }
    // OnOk (sub_789690): the booster's record (one of it used up, or gone), S2C 802 [record][u32 waiting].
    const BagRecord rec = bag_decrease(b, 1);
    if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
    else               server_.item_store().SetAmount(rec.uid, rec.count);
    bag_apply(rec);
    PacketBuffer p(static_cast<Opcode>(0x0322), is_paula());
    write_bag_record(p, rec);
    p.write<uint32_t>(static_cast<uint32_t>(wait));
    send(p);
    LOG_INFO("%s: %s (item %u, %d waiting)", char_name_, "hItemMegaphoneBooster::OnOk", d.id, wait);
    return 2;
}

// uspm_insert_broadcast_message: the messages still waiting (message_flag <= 1), then the row {char, island 0,
// message, flag 1 registered, type 1 normal}; 101 when it fails.
int cGameServer::megaphone_submit(uint32_t char_uid, const std::string& message_utf8, int32_t& wait)
{
    wait = 0;
    try {
        DBTransaction tx(db_);
        const auto rows = db_.query("SELECT COUNT(*) AS n FROM broadcast WHERE message_flag <= 1");
        if (!rows.empty()) wait = std::atoi(rows[0].at("n").c_str());
        const std::string msg = utf8_cut(message_utf8, 300);
        const uint64_t code = db_.insert("INSERT INTO broadcast (char_uid, island_code, message, message_flag, "
                                         "broadcast_type) VALUES (%u, 0, '%s', 1, 1)", char_uid,
                                         db_.escape(msg).c_str());
        if (!code) return 101;
        tx.commit();
    } catch (const DBException& e) {
        LOG_ERROR("uspm_insert_broadcast_message (101), %u: %s", char_uid, e.what());
        return 101;
    }
    return 0;
}

void cGameServer::start_megaphone()
{
    if (mega_delay_ms_ <= 0) return;                                     // (the Master: a delay of 0 sends none)
    mega_timer_.expires_after(std::chrono::milliseconds(mega_delay_ms_));
    mega_timer_.async_wait([this](std::error_code ec) {
        if (ec) return;                                                  // cancelled on stop
        megaphone_tick();
        start_megaphone();
    });
}

// ProcessMegaphoneMessage for our one island: uspm_select_broadcast_message's row (message_flag 0..1 — the
// procedure has no ORDER BY: the clustered key's order), S2C 801 to every community connection, then
// uspm_update_broadcast_success (flag 2, the log).
void cGameServer::megaphone_tick()
{
    uint64_t code = 0;
    int type = 1;
    std::string msg;
    try {
        const auto rows = db_.query("SELECT broadcast_code, broadcast_type, message FROM broadcast "
                                    "WHERE message_flag BETWEEN 0 AND 1 ORDER BY broadcast_code LIMIT 1");
        if (rows.empty()) return;
        code = std::strtoull(rows[0].at("broadcast_code").c_str(), nullptr, 10);
        type = std::atoi(rows[0].at("broadcast_type").c_str());
        msg  = rows[0].at("message");
    } catch (const DBException& e) {
        LOG_ERROR("cMasterServer::ProcessMegaphoneMessage failure! %s", e.what());
        return;
    }
    const std::string wire = to_client_text(msg);
    std::vector<std::shared_ptr<SystemClientSession>> list;
    {
        std::lock_guard<std::mutex> lock(system_mutex_);
        for (auto& kv : system_sessions_) if (kv.second) list.push_back(kv.second);
    }
    int n = 0;
    for (auto& s : list) {
        if (!s->char_uid()) continue;                                    // (a ClientProxy logged in)
        PacketBuffer p(static_cast<Opcode>(0x0321), s->is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(type));
        p.write_bytes(reinterpret_cast<const uint8_t*>(wire.c_str()), wire.size() + 1);
        s->send(p);
        ++n;
    }
    LOG_INFO("SYSTEMSERVER  MEGAPHONE: %s (SendCnt:%d)", msg.c_str(), n);
    try {
        DBTransaction tx(db_);
        if (db_.execute("UPDATE broadcast SET message_flag = 2 WHERE message_flag BETWEEN 0 AND 1 AND "
                        "broadcast_code = %llu", static_cast<unsigned long long>(code)) == 1)
            db_.execute("INSERT INTO broadcast_log (broadcast_code, message_flag) VALUES (%llu, 2)",
                        static_cast<unsigned long long>(code));
        else
            LOG_ERROR("uspm_update_broadcast_success Fail, %llu", static_cast<unsigned long long>(code));
        tx.commit();
    } catch (const DBException& e) {
        LOG_ERROR("UpdateMegaphoneMessage() failure! %s", e.what());
    }
}

// OnCCmdSysMegaphone (MasterServer sub_40A5D0): the GameServer's 3699 [str header][str text] (sub_602CB0: a 3rd job
// sub_78A210, level 400 sub_75D390, ...) -> "[%s] %s - %s" (the header and the text cut at 160 bytes, the island's
// name between) -> uspm_event_insert_broadcast_message: a row {char 0, island 0, message, flag 0 (a GM message),
// type 0}; the queue sends it in its turn (megaphone_tick -> S2C 801 type 0: the client shows it as it is).
void cGameServer::sys_megaphone(const std::string& header_utf8, const std::string& text_utf8)
{
    std::string h = header_utf8, t = text_utf8;
    if (to_client_text(h).size() > 160) h = from_client_text(cp949_cut(to_client_text(h), 160));
    if (to_client_text(t).size() > 160) t = from_client_text(cp949_cut(to_client_text(t), 160));
    const std::string msg = utf8_cut("[" + h + "] " + mega_island_ + " - " + t, 300);
    try {
        if (!db_.insert("INSERT INTO broadcast (char_uid, island_code, message, message_flag, broadcast_type) "
                        "VALUES (0, 0, '%s', 0, 0)", db_.escape(msg).c_str())) {
            LOG_ERROR("cMasterServer::OnCCmdSysMegaphone failure! : spRet:101");
            return;
        }
    } catch (const DBException& e) {
        LOG_ERROR("cMasterServer::OnCCmdSysMegaphone failure! %s", e.what());
        return;
    }
    LOG_INFO("cMasterServer::OnCCmdSysMegaphone Succeed! %s", msg.c_str());
}

// uspm_update_broadcast_recovery (the Master's start): every player's message still queued (flag 1) gives its TBN
// Megaphone back — +1 on the character's item 19656 wherever it is, else a new one in its bag while it has fewer
// than 300 items (flag 3), else nothing (flag 4: the bag full) — with the log row. (GM messages, flag 0, stay.)
void cGameServer::megaphone_recover()
{
    try {
        DBTransaction tx(db_);
        const auto rows = db_.query("SELECT b.broadcast_code, b.char_uid FROM broadcast b JOIN characters c ON "
                                    "c.uid=b.char_uid WHERE b.message_flag=1");
        for (const auto& r : rows) {
            const unsigned long long code = std::strtoull(r.at("broadcast_code").c_str(), nullptr, 10);
            const uint32_t chr = static_cast<uint32_t>(std::strtoul(r.at("char_uid").c_str(), nullptr, 10));
            int flag = 3;
            const auto it = db_.query("SELECT uid FROM char_inventory WHERE char_uid=%u AND item_id=19656 LIMIT 1", chr);
            if (!it.empty()) {
                db_.execute("UPDATE char_inventory SET amount=amount+1 WHERE uid=%llu",
                            std::strtoull(it[0].at("uid").c_str(), nullptr, 10));
            } else {
                const auto n = db_.query("SELECT COUNT(*) AS n FROM char_inventory WHERE char_uid=%u AND item_where=1", chr);
                if (!n.empty() && std::atoi(n[0].at("n").c_str()) < 300) item_store_.AddItem(chr, 19656, 1);
                else flag = 4;
            }
            db_.execute("UPDATE broadcast SET message_flag=%d WHERE broadcast_code=%llu", flag, code);
            db_.execute("INSERT INTO broadcast_log (broadcast_code, message_flag) VALUES (%llu, %d)", code, flag);
            LOG_INFO("uspm_update_broadcast_recovery: message %llu of %u -> flag %d", code, chr, flag);
        }
        tx.commit();
    } catch (const DBException& e) {
        LOG_ERROR("uspm_update_broadcast_recovery (101) %s", e.what());
    }
}
