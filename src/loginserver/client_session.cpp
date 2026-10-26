#include "client_session.hpp"
#include "login_server.hpp"
#include "logger.hpp"
#include "interserver/interserver_opcodes.hpp"
#include "store/char_record_io.hpp"
#include <random>
#include <cstring>
#include <algorithm>
#include <chrono>

void ClientSession::begin_handshake()
{
    if (is_paula()) {
        LOG_INFO("ClientSession %u: Paula mode active, awaiting client handshake", session_id());
        return;
    }

    std::random_device rd;
    uint32_t seed = rd();
    set_crypt_key(seed);

    PacketBuffer pkt(Opcode::SESSION_ENCRYPT);
    pkt.write<uint32_t>(seed);
    send(pkt);
    LOG_INFO("ClientSession %u: handshake started (crypt_key=0x%08X)", session_id(), seed);
}

void ClientSession::on_packet(std::shared_ptr<Session> /*self*/, PacketBuffer pkt)
{
    Opcode op = pkt.opcode();
    LOG_DEBUG("ClientSession %u: recv 0x%04X (%s)",
              session_id(), static_cast<uint16_t>(op), opcode_name(op));

    switch (op) {
    case Opcode::C2S_SESSION_READY:   handle_session_ready(pkt);      break;
    case Opcode::C2S_SESSION_ACK:     handle_session_ack(pkt);        break;
    case Opcode::HEARTBEAT_A:
    case Opcode::HEARTBEAT_B:         handle_heartbeat(pkt, op);      break;
    case Opcode::C2S_CHAR_AUTH:       handle_auth_token(pkt);         break;
    case Opcode::CHAR_AUTH_ACK:       handle_char_list_request(pkt);  break;
    case Opcode::C2S_CHAR_CREATE:     handle_char_create(pkt);        break;
    case Opcode::C2S_CHAR_DELETE:     handle_char_delete(pkt);        break;
    case Opcode::C2S_CHAR_SELECT:     handle_char_select(pkt);        break;
    case Opcode::C2S_SLOT_QUERY:      handle_slot_query(pkt);         break;
    case Opcode::C2S_CAPSULE_QUERY:
    case Opcode::C2S_CAPSULE_LIST:
        LOG_DEBUG("ClientSession %u: capsule/slot query 0x%04X, ignored",
                  session_id(), static_cast<uint16_t>(op));
        break;
    default:
        LOG_WARN("ClientSession %u: unhandled 0x%04X (state=%d)",
                 session_id(), static_cast<uint16_t>(op), static_cast<int>(state_));
        break;
    }
}

void ClientSession::on_close(std::shared_ptr<Session> /*self*/)
{
    LOG_INFO("ClientSession %u: disconnected (account=%u, state=%d)",
             session_id(), account_id_, static_cast<int>(state_));
}

void ClientSession::handle_session_ready(PacketBuffer& /*pkt*/)
{
    send_session_open();
}

void ClientSession::handle_session_ack(PacketBuffer& /*pkt*/)
{
    // Hardening (not retail): this pre-login handshake step must not reset a logged-in session to
    // WaitingAuth (that would let a second 0x07D0 switch its account); the Paula client never sends it.
    if (state_ != ClientState::WaitingAuth) return;
    state_ = ClientState::WaitingAuth;
    PacketBuffer p1(Opcode::SESSION_PARAM_1);
    p1.write<uint32_t>(0);
    send(p1);
}

void ClientSession::handle_auth_token(PacketBuffer& pkt)
{
    // Hardening (not retail): one token per connection. A second 0x07D0 would switch this session to
    // another account in the middle of its char select; the client sends it once, on connect.
    if (state_ != ClientState::WaitingAuth) {
        LOG_WARN("ClientSession %u: repeated auth token (account %u already logged in); ignored",
                 session_id(), account_id_);
        return;
    }

    if (Logger::instance().enabled(LogLevel::Debug)) {
        const uint8_t* d = pkt.data() + pkt.rpos();
        size_t n = pkt.size() - pkt.rpos();
        std::string hex;
        for (size_t i = 0; i < n && i < 64; ++i) {
            char b[4]; snprintf(b, sizeof(b), "%02X ", d[i]); hex += b;
        }
        LOG_DEBUG("ClientSession %u: 0x07D0 payload (%zu bytes): %s", session_id(), n, hex.c_str());
    }

    // Auth token (C2S 0x07D0): [0..7] carry-over, [8..11] account_id, [12..15] token, [16..20] zeros
    uint32_t incoming_account_id = 0;
    uint32_t incoming_token      = 0;
    try {
        pkt.skip(8);
        incoming_account_id = pkt.read<uint32_t>();
        incoming_token      = pkt.read<uint32_t>();
    } catch (const std::exception& e) {
        LOG_WARN("ClientSession %u: auth_token parse error: %s", session_id(), e.what());
        close();
        return;
    }

    // Validate by TOKEN ONLY and take the account_id FROM the token row. The client self-reports
    // an account_id at [8..11], but it is NOT reliable: it always sends 1 regardless of which
    // account actually logged in (confirmed in captures — account '2' still sent account_id=1),
    // so requiring it to match broke the 2nd account. The token is the real source of truth: it is
    // a unique one-time value the AuthServer issued together with the correct account_id, so we
    // trust the token's account_id and ignore the client's self-reported one.
    auto& db = server_.db();
    DBResult rows;
    try {
        rows = db.query(
            "SELECT account_id, gm_level FROM login_tokens "
            "WHERE token=%u AND expires_at > NOW()",
            incoming_token
        );
        if (!rows.empty())
            db.execute("DELETE FROM login_tokens WHERE token=%u", incoming_token);
    } catch (const std::exception& e) {
        LOG_ERROR("ClientSession %u: auth token lookup failed: %s", session_id(), e.what());
        close();
        return;
    }

    if (rows.empty()) {
        LOG_WARN("ClientSession %u: invalid or expired auth token 0x%08X (client-reported account %u)",
                 session_id(), incoming_token, incoming_account_id);
        close();
        return;
    }

    account_id_ = static_cast<uint32_t>(std::stoul(rows[0].at("account_id")));
    gm_level_   = static_cast<uint8_t>(std::stoi(rows[0].at("gm_level")));
    if (account_id_ != incoming_account_id)
        LOG_INFO("ClientSession %u: token resolved account_id=%u (client self-reported %u)",
                 session_id(), account_id_, incoming_account_id);
    state_      = ClientState::CharSelect;
    login_done();

    LOG_INFO("ClientSession %u: authenticated as account %u (gm=%u)",
             session_id(), account_id_, gm_level_);

    send_auth_ack();

    // Ask the Master for this account's char list; build 0x07D2 when it replies. A reply that ends
    // early is the Master's failure answer (its store threw): the client cannot be shown a list then.
    auto self = shared_from_this();
    paula::inter::StreamWriter w;
    w.u32(account_id_);
    server_.master_request(paula::inter::MREQ_CHAR_LIST, w.data(),
        [this, self](paula::inter::StreamReader& r) {
            uint8_t usable = r.u8();
            uint8_t count = r.u8();
            std::vector<CharRecord> chars;
            for (uint8_t i = 0; i < count && r.ok(); ++i) chars.push_back(paula::unpack_char(r));
            const bool ok = r.ok();
            schedule_delayed(std::chrono::milliseconds(0),
                [this, self, ok, usable, chars = std::move(chars)]() mutable {
                    if (!ok) {
                        LOG_ERROR("ClientSession %u: the Master could not load account %u's characters; "
                                  "closing", session_id(), account_id_);
                        close();
                        return;
                    }
                    usable_slot_ = usable;
                    chars_ = std::move(chars);
                    send_world_ack();
                });
        });
}

void ClientSession::handle_char_list_request(PacketBuffer& /*pkt*/)
{
    LOG_DEBUG("ClientSession %u: 0x07EC C2S received, ignored (chars in NLT)", session_id());
}

namespace {

// LoginServer sub_4F92C0: a name the LoginServer lets through (CP949 bytes, as the client's sub_812BC0):
// not empty, not starting with '.', '\'' or '/'; no byte <= 0x20, '`', ',' or '\''; a byte >= 0x80
// followed by another is a double-byte character, refused when either byte is 0xFF, the lead is
// >= 0xC7 with a trail in 0x41..0xA0, the lead is 0xC6 with a trail in 0x53..0xA0, the lead is 0xC9
// or 0xFE, or it is 0xA4D4; a '.' as the last character is refused; 4..16 bytes long.
bool login_name_ok(const std::string& n)
{
    if (n.empty()) return false;
    const unsigned char first = static_cast<unsigned char>(n[0]);
    if (first == '.' || first == '\'' || first == '/') return false;
    size_t i = 0;
    for (; i < n.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(n[i]);
        if (c <= 0x20 || c == '`' || c == ',' || c == '\'') return false;
        if (i + 1 < n.size()) {
            if (c >= 0x80) {
                const unsigned char t = static_cast<unsigned char>(n[i + 1]);
                if (c == 0xFF || t == 0xFF) return false;
                if (c >= 0xC7 && t >= 0x41 && t <= 0xA0) return false;
                if (c == 0xC6 && t >= 0x53 && t <= 0xA0) return false;
                if (c == 0xC9 || c == 0xFE) return false;
                if (((c << 8) | t) == 0xA4D4) return false;
                ++i;
            }
        } else if (c == '.') {
            return false;
        }
    }
    return i >= 4 && i <= 16;
}

} // namespace

// LCMD_NEW_CHR (LoginServer sub_4065C0): C2S 2006 [name][u16 type][u8 hair][u8 job][u8 f_type]
// [u8 pts_p][u8 pts_m][u8 pts_s][u8 pts_c] (client sub_75C980). The name check, then the first free
// slot, then CCMD_NEW to the Master with the start zone and return town 148 (Coral Town — every
// never-played Paula character is saved there). A refusal is S2C 2007 [u32 code] (client sub_760E40:
// the NetworkErrorString Comment of the code).
void ClientSession::handle_char_create(PacketBuffer& pkt)
{
    // Hardening (not retail): only after the token step (the client shows char select only then).
    if (state_ != ClientState::CharSelect) {
        LOG_WARN("ClientSession %u: char create before login; ignored", session_id());
        return;
    }
    CharRecord chr{};
    try {
        std::string name;
        while (pkt.rpos() < pkt.size()) {
            const char c = static_cast<char>(pkt.read<uint8_t>());
            if (!c) break;
            name += c;
        }
        if (name.size() > 19) name.resize(19);             // lstrcpynA(String1, name, 20)
        if (!login_name_ok(name)) {
            LOG_INFO("LCMD_NEW_CHR_ACK_CANCEL: index: %u, name:<%s>", session_id(), Logger::printable(name).c_str());
            send_create_fail(1019);
            return;
        }
        std::memcpy(chr.name, name.data(), std::min<size_t>(name.size(), 17));
        chr.type   = static_cast<int16_t>(pkt.read<uint16_t>());
        chr.hair   = pkt.read<uint8_t>();
        chr.job    = pkt.read<uint8_t>();
        chr.f_type = pkt.read<uint8_t>();
        chr.pts_p  = pkt.read<uint8_t>();
        chr.pts_m  = pkt.read<uint8_t>();
        chr.pts_s  = pkt.read<uint8_t>();
        chr.pts_c  = pkt.read<uint8_t>();
    } catch (const std::exception& e) {
        LOG_ERROR("ClientSession %u: char_create parse error: %s", session_id(), e.what());
        return;
    }

    // The first free slot of the account's.
    int slot = -1;
    for (int i = 0; i < usable_slot_ && slot < 0; ++i) {
        bool used = false;
        for (const CharRecord& c : chars_) if (c.slot == i) { used = true; break; }
        if (!used) slot = i;
    }
    if (slot < 0) {
        LOG_INFO("Character Slot full: max: %u", static_cast<unsigned>(usable_slot_));
        LOG_INFO("LCMD_NEW_CHR_ACK_CANCEL: index: %u", session_id());
        send_create_fail(1020);
        return;
    }
    chr.owner = account_id_;
    chr.slot  = static_cast<uint8_t>(slot);
    chr.zone  = 148;
    chr.home  = 148;

    auto self = shared_from_this();
    paula::inter::StreamWriter w;
    paula::pack_char(w, chr);
    server_.master_request(paula::inter::MREQ_CHAR_CREATE, w.data(),
        [this, self](paula::inter::StreamReader& r) {
            uint32_t code = r.u32();
            CharRecord made{};
            if (!code) made = paula::unpack_char(r);
            if (!r.ok()) code = 9996;                    // no (whole) answer from the Master: not made
            schedule_delayed(std::chrono::milliseconds(0),
                [this, self, code, made]() {
                    if (code) {                                  // LoginServer sub_406AC0
                        LOG_INFO("LCMD_NEW_CHR_ACK_CANCEL: index: %u,error:%u", session_id(), code);
                        send_create_fail(code);
                        return;
                    }
                    // LoginServer sub_406860: the slot taken meanwhile -> 1065 (the character stays made)
                    for (const CharRecord& c : chars_)
                        if (c.slot == made.slot) {
                            LOG_INFO("LCMD_NEW_CHR_ACK_CANCEL: index: %u", session_id());
                            send_create_fail(1065);
                            return;
                        }
                    chars_.push_back(made);
                    LOG_INFO("LCMD_NEW_CHR_ACK_OK: index: %u, name: <%s>, uid:%u", session_id(), made.name, made.uid);
                    PacketBuffer res(Opcode::CHAR_CREATE_OK, is_paula());
                    send_char_entry(res, made);
                    send(res);
                });
        });
}

void ClientSession::send_create_fail(uint32_t code)
{
    PacketBuffer fail(Opcode::CHAR_CREATE_FAIL, is_paula());
    fail.write<uint32_t>(code);
    send(fail);
}

// LCMD_DEL_CHR (LoginServer sub_406BB0): C2S 2009 [u64 char uid]... A character not on the list -> S2C 2010
// [1025]; else CCMD_DEL with the client's IP (sub_4046E0) -> 2011 (empty, sub_406D30) or 2010 [20] (sub_406F40).
void ClientSession::handle_char_delete(PacketBuffer& pkt)
{
    // Hardening (not retail): only after the token step (the client shows char select only then).
    if (state_ != ClientState::CharSelect) {
        LOG_WARN("ClientSession %u: char delete before login; ignored", session_id());
        return;
    }
    uint32_t char_uid = pkt.read<uint32_t>();
    auto it = std::find_if(chars_.begin(), chars_.end(),
                           [char_uid](const CharRecord& c){ return c.uid == char_uid; });
    if (it == chars_.end()) {
        LOG_INFO("LCMD_DEL_CHR_ACK_CANCEL: index: %u, No character:uid:%u", session_id(), char_uid);
        PacketBuffer fail(Opcode::CHAR_DELETE_FAIL, is_paula());
        fail.write<uint32_t>(1025);
        send(fail);
        return;
    }

    auto self = shared_from_this();
    paula::inter::StreamWriter w;
    w.u32(account_id_);
    w.u32(char_uid);
    w.str(remote_address());
    server_.master_request(paula::inter::MREQ_CHAR_DELETE, w.data(),
        [this, self, char_uid](paula::inter::StreamReader& r) {
            uint8_t ok = r.u8();
            schedule_delayed(std::chrono::milliseconds(0),
                [this, self, char_uid, ok]() {
                    if (!ok) {
                        LOG_INFO("LCMD_DEL_CHR_ACK_CANCEL: index: %u, uid:%u", session_id(), char_uid);
                        PacketBuffer fail(Opcode::CHAR_DELETE_FAIL, is_paula());
                        fail.write<uint32_t>(20);
                        send(fail);
                        return;
                    }
                    auto it2 = std::find_if(chars_.begin(), chars_.end(),
                                            [char_uid](const CharRecord& c){ return c.uid == char_uid; });
                    if (it2 != chars_.end()) chars_.erase(it2);
                    LOG_INFO("ClientSession %u: deleted character uid=%u", session_id(), char_uid);
                    // Delete-OK = 2011 (0x07DB, EMPTY) — client case 2011 rebuilds the list.
                    PacketBuffer okp(Opcode::CHAR_DELETE_OK, is_paula());
                    send(okp);
                });
        });
}

void ClientSession::handle_char_select(PacketBuffer& pkt)
{
    uint32_t char_uid = pkt.read<uint32_t>();
    // Only a character of this account's own list (the Master's, for the token's account; never a
    // deleted one).
    auto it = std::find_if(chars_.begin(), chars_.end(),
                           [char_uid](const CharRecord& c){ return c.uid == char_uid; });
    if (state_ != ClientState::CharSelect || it == chars_.end() || it->owner != account_id_ ||
        it->deleted || it->authority == 2) {
        LOG_WARN("ClientSession %u: select of char %u refused (not on account %u's list)",
                 session_id(), char_uid, account_id_);
        return;
    }

    // Hardening (not retail): the game ticket. Retail hands the selected character to its GameServer
    // over the server bus; ours only learns the char uid from the client's first packet (C2S 0x0004),
    // so it admits a character only against this one-time ticket: this char, this account, the
    // client's IP, valid 300 s. Written before the client is told where the GameServer is.
    try {
        auto& db = server_.db();
        db.execute("REPLACE INTO game_tickets (char_uid, account_id, client_ip, expires_at) "
                   "VALUES (%u, %u, '%s', NOW() + INTERVAL 300 SECOND)",
                   char_uid, account_id_, db.escape(remote_address()).c_str());
    } catch (const std::exception& e) {
        LOG_ERROR("ClientSession %u: game ticket for char %u not stored (%s); select dropped",
                  session_id(), char_uid, e.what());
        return;
    }
    send_map_server_info(*it);
}

void ClientSession::handle_slot_query(PacketBuffer& /*pkt*/)
{
    PacketBuffer resp(Opcode::SLOT_TICKET_COUNT, is_paula());
    resp.write<uint16_t>(cLoginServer::BASE_CHAR_SLOTS);
    send(resp);
}

void ClientSession::handle_heartbeat(PacketBuffer& /*pkt*/, Opcode /*op*/)
{
    PacketBuffer ack(Opcode::HEARTBEAT_ACK, is_paula());
    send(ack);
}

// ─── Responses ───────────────────────────────────────────────────────────────

void ClientSession::send_session_open()
{
    PacketBuffer pkt(Opcode::SESSION_OPEN);
    pkt.write<uint32_t>(session_id());
    send(pkt);
}

void ClientSession::send_auth_ack()
{
    PacketBuffer pkt(Opcode::CHAR_AUTH_ACK, is_paula());
    static const uint8_t kAckData[40] = {
        0x42, 0x72, 0x05, 0x51, 0x38, 0x20, 0xDB, 0x2B,
        0x27, 0x2A, 0xD5, 0x64, 0x85, 0x08, 0xFC, 0x04,
        0x0A, 0xC2, 0xBF, 0xB6, 0xEF, 0x2F, 0x51, 0x60,
        0xF3, 0x3A, 0xF4, 0x2F, 0xD5, 0x59, 0x5F, 0x58,
        0xD0, 0xED, 0x8B, 0xA9, 0x76, 0x54, 0x32, 0x10
    };
    pkt.write_bytes(kAckData, 40);
    for (int i = 0; i < 360; ++i) pkt.write<uint8_t>(0);
    pkt.write<uint16_t>(40);
    pkt.write<uint16_t>(4585);
    send(pkt);
}

void ClientSession::send_world_ack()
{
    // 0x07D2 — combined world/char list; NST stores 156-byte entries into NLT, then NCST reads them.
    PacketBuffer pkt(Opcode::PAULA_SERVER_LIST, is_paula());
    pkt.write<uint8_t>(static_cast<uint8_t>(chars_.size()));

    for (auto& c : chars_)
        send_char_entry(pkt, c);

    pkt.write<uint32_t>(0);
    pkt.write<uint32_t>(0);
    pkt.write<uint8_t>(0);
    pkt.write<uint8_t>(0);
    pkt.write<uint8_t>(0);
    pkt.write<uint8_t>(usable_slot_);       // the open slots (the client locks slots >= this)

    send(pkt);
    LOG_INFO("ClientSession %u: sent 0x07D2 with %zu char entries", session_id(), chars_.size());
}

void ClientSession::send_char_entry(PacketBuffer& pkt, const CharRecord& c)
{
    pkt.write<uint32_t>(c.uid);
    pkt.write<uint32_t>(0);
    pkt.write<uint8_t>(c.slot);

    pkt.write_string_fixed(c.name, 20);

    // The tiers (MasterServer GetByOwnerFromDb columns; client sub_74E090 / sub_724010): the client
    // shows tier job_state — its model goes to +29 (type_2 from +31, type_3 from +140), its class
    // name from JobTypeInfo[+34 / +35 / +142], its hair palette from +37 / +38 / +143.
    pkt.write<uint16_t>(static_cast<uint16_t>(c.type));     // +29 type
    pkt.write<uint16_t>(static_cast<uint16_t>(c.type_2));   // +31 type_2
    pkt.write<uint8_t>(c.f_type);                           // +33 f_type
    pkt.write<uint8_t>(static_cast<uint8_t>(c.job));        // +34 job
    pkt.write<uint8_t>(c.job_2);                            // +35 job_2
    pkt.write<uint8_t>(c.job_state ? c.job_state : 1);      // +36 job_state
    pkt.write<uint8_t>(c.hair);                             // +37 hair
    pkt.write<uint8_t>(c.hair_2);                           // +38 hair_2
    pkt.write<uint32_t>(static_cast<uint32_t>(c.money));
    pkt.write<uint16_t>(static_cast<uint16_t>(c.level));

    // +45: the 23 equip positions' looks (MasterServer: entry + 45 + 4*item_pos; client sub_74E090 builds
    // the model from them, sub_746880(type, +45, 23), and lays positions 10..19 over it)
    for (uint32_t id : c.equip) pkt.write<uint32_t>(id);
    pkt.write<uint8_t>(c.guild_state);                      // +137
    pkt.write<uint8_t>(c.is_guild_chief);                   // +138
    pkt.write<uint8_t>(c.tutorial_step);                    // +139
    pkt.write<uint16_t>(static_cast<uint16_t>(c.type_3));   // +140 type_3
    pkt.write<uint8_t>(c.job_3);                            // +142 job_3
    pkt.write<uint8_t>(c.hair_3);                           // +143 hair_3
    pkt.write<uint8_t>(c.hack_index);                       // +144
    pkt.write<uint8_t>(c.awakening_flag);                   // +145 (the client's AwakenMark)
    pkt.write<uint32_t>(c.couple_code);                     // +146 u64 couple code
    pkt.write<uint32_t>(0);
    pkt.write<uint16_t>(static_cast<uint16_t>(c.tm_level)); // +154 (client: level >= 400 and this >= 500
                                                            //  -> the 5th slot frame)
}

void ClientSession::send_map_server_info(const CharRecord& chr)
{
    // 0x07DE: GameServer address for the selected char (then 0x0003 connect-now).
    PacketBuffer gs(Opcode::CHAR_SELECT_MAP, is_paula());
    gs.write<uint32_t>(chr.uid);
    gs.write<uint32_t>(0);
    gs.write<uint32_t>(static_cast<uint32_t>(static_cast<uint16_t>(chr.zone)));
    gs.write<uint16_t>(1);
    const std::string& ip = server_.map_ip();
    gs.write_bytes(reinterpret_cast<const uint8_t*>(ip.c_str()), ip.size() + 1);
    gs.write<uint32_t>(server_.map_port());
    for (int i = 0; i < 14; ++i) gs.write<uint8_t>(0);
    send(gs);

    PacketBuffer ok(Opcode::CHAR_SELECT_OK, is_paula());
    ok.write<uint16_t>(0);
    ok.write<uint32_t>(account_id_);
    ok.write<uint32_t>(0);
    send(ok);

    LOG_INFO("ClientSession %u: char selected uid=%u, redirecting to game %s:%u",
             session_id(), chr.uid, server_.map_ip().c_str(), server_.map_port());
}
