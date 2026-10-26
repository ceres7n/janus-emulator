#include "auth_session.hpp"
#include "auth_server.hpp"
#include "logger.hpp"
#include <random>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  Login flow (Paula protocol, the retail FirstLoginSvrW):
//    CLIENT → Server  HANDSHAKE_PAULA 0x2CED      account name, "dummy" password, launch key
//    Server → CLIENT  CTrLoginUserAck 11502       token + island list
//                  or CTrLoginUserNak 11503       refused; the connection then closes
//    CLIENT → Server  CTrReserveWorldReq 11505    the picked island
//    Server → CLIENT  CTrReserveWorldAck 11506    its LoginServer address (or Nak 11507)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// CTrLoginUserNak codes. The client shows the same "login failed" text for any code (sub_740E60).
// The account DB procedure that gave retail its per-case codes is not in the backup, so:
constexpr uint32_t kNakLoginFailed = 20055;   // sub_403A94's own code for a failed login query
constexpr uint32_t kNakBlocked     = 60012;   // sub_403A94's blocked account (retail also sends 11508
                                              // CTrLoginUserNakBlock first; its layout is not verified)

} // namespace

void AuthSession::begin_handshake()
{
    // Season 2 / Paula mode relies on the client to send the first packet (LoginRequest).
    // The Trickster client will freeze if we force an unexpected 0x000F handshake onto it.
    LOG_DEBUG("AuthSession %u: awaiting encrypted HANDSHAKE_PAULA (0x2CED)", session_id());
}

void AuthSession::on_packet(std::shared_ptr<Session>, PacketBuffer pkt)
{
    Opcode op = pkt.opcode();
    LOG_DEBUG("AuthSession %u: recv opcode 0x%04X (%s)",
              session_id(), static_cast<uint16_t>(op), opcode_name(op));

    switch (op) {
    case Opcode::HANDSHAKE_PAULA:
        handle_handshake_paula(pkt);
        break;
    case Opcode::C2S_SERVER_SELECT:
        handle_server_select(pkt);
        break;
    case Opcode::HEARTBEAT_A:
    case Opcode::HEARTBEAT_B:
        handle_heartbeat(pkt, op);
        break;
    default:
        LOG_WARN("AuthSession %u: unhandled opcode 0x%04X in state %d",
                 session_id(), static_cast<uint16_t>(op), static_cast<int>(state_));
        break;
    }
}

void AuthSession::on_close(std::shared_ptr<Session> /*self*/)
{
    state_ = LoginState::Closed;
    LOG_INFO("AuthSession %u: disconnected", session_id());
}

void AuthSession::handle_handshake_paula(PacketBuffer& pkt)
{
    if (state_ != LoginState::Handshake) return;

    // Payload layout (46 bytes, confirmed from RE):
    //   [0..18]  username (19 bytes, null-padded)
    //   [19..37] password (19 bytes, always "dummy" — real auth via session key)
    //   [38..41] session_key_lo (uint32_t LE)
    //   [42..45] session_key_hi (uint32_t LE)
    std::string username;
    uint32_t key_lo = 0, key_hi = 0;
    try {
        username = pkt.read_string_fixed(19);
        pkt.read_string_fixed(19);              // the password: not checked (no passwords in v0.1)
        key_lo   = pkt.read<uint32_t>();
        key_hi   = pkt.read<uint32_t>();
    } catch (...) {
        LOG_WARN("AuthSession %u: failed to parse HANDSHAKE_PAULA body; closing", session_id());
        state_ = LoginState::Closed;
        close();
        return;
    }

    const std::string shown = Logger::printable(username);
    LOG_INFO("AuthSession %u: login attempt for '%s' from %s (key=%08X%08X)",
             session_id(), shown.c_str(), remote_address().c_str(), key_hi, key_lo);

    // Resolve the account from the username the client sent (from the launcher's account field).
    // The client ECHOES back whatever account_id we put in the login response (it sends it in
    // C2S 0x07D0), so the account identity is fully decided here. There is no password in v0.1:
    // the account name is the key. An unknown name, or a banned account (banned = 1 with no end
    // date or one still ahead), is refused with CTrLoginUserNak and the connection closes.
    account_.account_id = 0;
    account_.gm_level   = 0;
    uint32_t refuse = 0;
    try {
        auto& db = server_.db();
        auto rows = db.query(
            "SELECT account_id, gm_level, banned, (ban_until IS NULL OR ban_until > NOW()) AS ban_active "
            "FROM accounts WHERE username='%s' LIMIT 1",
            db.escape(username).c_str());
        if (rows.empty()) {
            LOG_INFO("AuthSession %u: unknown account '%s'; refused", session_id(), shown.c_str());
            refuse = kNakLoginFailed;
        } else if (rows[0].at("banned") == "1" && rows[0].at("ban_active") == "1") {
            LOG_INFO("AuthSession %u: account '%s' is banned; refused", session_id(), shown.c_str());
            refuse = kNakBlocked;
        } else {
            account_.account_id = static_cast<uint32_t>(std::stoul(rows[0].at("account_id")));
            account_.gm_level   = static_cast<uint8_t>(std::stoi(rows[0].at("gm_level")));
        }
    } catch (const std::exception& e) {
        LOG_ERROR("AuthSession %u: account lookup failed: %s", session_id(), e.what());
        refuse = kNakLoginFailed;
    }
    if (refuse || account_.account_id == 0) {
        send_login_nak(refuse ? refuse : kNakLoginFailed);
        return;
    }

    // Generate a one-time auth token for the char server handshake.
    // Stored in login_tokens with a 60-minute TTL (the player may sit in the server select); the
    // LoginServer validates and deletes it on connect (C2S 0x07D0). Expired rows are purged here.
    try {
        std::random_device rd;
        account_.auth_token = rd();
        auto& db = server_.db();
        db.execute("DELETE FROM login_tokens WHERE expires_at <= NOW()");
        db.execute(
            "REPLACE INTO login_tokens (token, account_id, gm_level, expires_at) "
            "VALUES (%u, %u, %u, DATE_ADD(NOW(), INTERVAL 60 MINUTE))",
            account_.auth_token, account_.account_id, account_.gm_level
        );
        LOG_DEBUG("AuthSession %u: auth token generated (account=%u token=0x%08X)",
                  session_id(), account_.account_id, account_.auth_token);
    } catch (const std::exception& e) {
        LOG_ERROR("AuthSession %u: login token not stored: %s", session_id(), e.what());
        send_login_nak(kNakLoginFailed);
        return;
    }

    // The account's last login (not needed for the login itself: a failure only warns).
    try {
        auto& db = server_.db();
        db.execute("UPDATE accounts SET last_login=NOW(), last_ip='%s' WHERE account_id=%u",
                   db.escape(remote_address()).c_str(), account_.account_id);
    } catch (const std::exception& e) {
        LOG_WARN("AuthSession %u: last_login / last_ip not updated: %s", session_id(), e.what());
    }

    state_ = LoginState::Authenticated;
    account_.username = username;
    login_done();
    LOG_INFO("AuthSession %u: '%s' -> account_id=%u (gm=%u)",
             session_id(), shown.c_str(), account_.account_id, account_.gm_level);

    send_login_response();
}

void AuthSession::handle_heartbeat(PacketBuffer& /*pkt*/, Opcode op)
{
    // In Paula mode all S2C packets must be Paula-encrypted so the client's
    // 9-byte framed read path stays aligned.
    PacketBuffer ack(Opcode::HEARTBEAT_ACK, is_paula());
    send(ack);
}

// ─── Responses ───────────────────────────────────────────────────────────────

void AuthSession::send_login_nak(uint32_t code)
{
    // CTrLoginUserNak 11503 [u32 code] (FirstLoginSvrW sub_403A30 / sub_403FF5 send it with the code
    // sub_403A94 / sub_404059 return): the client (sub_740EF0 -> sub_740E60) shows the LoginStrings
    // alert Alert_CannotConnectServer in a "LOGIN ERROR" box and ends its login task. The connection
    // closes once the packet has gone out.
    state_ = LoginState::Closed;
    PacketBuffer pkt(Opcode::PAULA_LOGIN_FAIL, true);
    pkt.write<uint32_t>(code);
    send(pkt);
    close_after_flush();
    LOG_INFO("AuthSession %u: login refused (11503, code %u)", session_id(), code);
}

// ─── Season 2 / Paula (KTO original) responses ───────────────────────────────

// One 75-byte island row of CTrLoginUserAck (FirstLoginSvrW sub_4037D1; the client reads it with
// sub_724260 into the struct sub_723AB0 clears):
//   [0]       u8        status (record +4)   kept by the client, never read
//   [1..2]    u16       world                the client groups the rows by it (sub_723EF0)
//   [3..4]    u16       island no            echoed back in CTrReserveWorldReq
//   [5..37]   char[33]  world name           "Trickster <name>" window title after the connect (sub_72F2C0)
//   [38..70]  char[33]  island name          the ServerSlotBtnN caption (sub_72E850 -> sub_7337A0)
//   [71..74]  u16, u16  record +96 / +98     never read by the client; 0 here
static void write_island_row(PacketBuffer& pkt, const IslandInfo& is)
{
    pkt.write<uint8_t>(is.open ? 1 : 0);
    pkt.write<uint16_t>(is.world);
    pkt.write<uint16_t>(is.island);
    pkt.write_string_fixed(is.world_name, 33);
    pkt.write_string_fixed(is.island_name, 33);
    pkt.write<uint16_t>(0);
    pkt.write<uint16_t>(0);
}

void AuthSession::send_login_response()
{
    // CTrLoginUserAck 11502 (FirstLoginSvrW sub_403A94; client NewLoginTask case 11502 -> sub_724440,
    // then the server-select task): [u32][u64 auth][u8 n] + n × 75-byte island rows.
    // The rows are the world table in slot order: every row whose record +100 is set, and the others
    // too for a user with the +764 flag (a column of uspfl_account_update_user_authority; here a GM
    // account). A closed island is listed as well; picking it is refused.
    // The client picks its window by n (sub_732AC0): n == 2 opens uServerSelectWindowView (two fixed
    // ConnectBtn1/2, no text from the rows at all); any other n opens uManyServerSelectWindowView,
    // whose six ServerSlotBtn1..6 show the island names. It writes those captions unbounded
    // (window[184 + i]), so more than 6 rows would run over the window: capped at 6 here.
    std::vector<const IslandInfo*> rows;
    for (const auto& is : server_.islands())
        if (is.open_to_all || account_.gm_level > 0) rows.push_back(&is);
    if (rows.size() > 6) rows.resize(6);

    PacketBuffer pkt(Opcode::PAULA_LOGIN_RESULT, true); // 0x2CEE
    pkt.write<uint32_t>(1);                    // server_group_type = 1 (normal)
    pkt.write<uint32_t>(account_.auth_token);  // auth_token_lo
    pkt.write<uint32_t>(0);                    // auth_token_hi
    pkt.write<uint8_t>(static_cast<uint8_t>(rows.size()));
    for (const IslandInfo* is : rows) write_island_row(pkt, *is);
    send(pkt);
    LOG_INFO("AuthSession %u: sent login result 0x2CEE (account=%u, %zu islands)",
             session_id(), account_.account_id, rows.size());
    // Do NOT send 0x7D2 here: the login task takes 0x7D2 as the char list (sub_42DEB0(5) ->
    // NewCharSelectTask) and skips the server select.
}

void AuthSession::handle_server_select(PacketBuffer& pkt)
{
    // CTrReserveWorldReq 11505 [u16 world][u16 island no] (client sub_72F200) -> FirstLoginSvrW
    // sub_404893. Refused with CTrReserveWorldNak 11507 when the session has not logged in (20087),
    // already holds a reservation (60014), names no row of the table (60007; the whole table is
    // searched, GM-only rows included) or a closed island (60008). Otherwise the reservation is taken
    // and CTrReserveWorldAck 11506 sends that row's LoginServer address.
    // Not modelled: the retail uspfl_account_game_update_select_world call in between, whose
    // failure codes (e.g. 60015 = full) refuse the pick; the account DB holding it is not in the backup.
    uint16_t world = 0, island = 0;
    try {
        world  = pkt.read<uint16_t>();
        island = pkt.read<uint16_t>();
    } catch (...) {
        LOG_WARN("AuthSession %u: failed to parse C2S_SERVER_SELECT", session_id());
    }
    LOG_INFO("AuthSession %u: server select world=%u island=%u", session_id(), world, island);

    if (state_ != LoginState::Authenticated) { send_reserve_world_nak(20087); return; }
    if (world_reserved_)                     { send_reserve_world_nak(60014); return; }

    const IslandInfo* row = nullptr;
    for (const auto& is : server_.islands())
        if (is.world == world && is.island == island) { row = &is; break; }
    if (!row)       { send_reserve_world_nak(60007); return; }
    if (!row->open) { send_reserve_world_nak(60008); return; }

    world_reserved_ = true;
    send_channel_info(*row);
}

void AuthSession::send_reserve_world_nak(uint32_t code)
{
    // CTrReserveWorldNak 11507 [u32 code]: the client (sub_732670) shows "Alert_SelectedServerToFull"
    // for 60015, else the NetworkErrorString comment for the code ("Undefined Comment - (code)" when the
    // table has no row for it, as for these 600xx codes).
    PacketBuffer pkt(Opcode::PAULA_RESERVE_WORLD_NAK, true);
    pkt.write<uint32_t>(code);
    send(pkt);
    LOG_INFO("AuthSession %u: server select refused (%u)", session_id(), code);
}

void AuthSession::send_channel_info(const IslandInfo& is)
{
    // CTrReserveWorldAck 11506: the row's IP (char[16]) + port (u16) = 18 bytes (client sub_7325D0,
    // then sub_72F2C0 connects there with C2S 2000).
    PacketBuffer pkt(Opcode::PAULA_CHANNEL_INFO, true);
    pkt.write_string_fixed(is.ip, 16);
    pkt.write<uint16_t>(is.port);
    send(pkt);
    LOG_INFO("AuthSession %u: sent channel info (0x2CF2) -> '%s' %s:%u",
             session_id(), is.island_name.c_str(), is.ip.c_str(), is.port);
}
