#pragma once
#include <memory>
#include "session.hpp"
#include "packet.hpp"
#include "opcodes.hpp"
#include "database.hpp"
#include <string>

// Forward declare
class cAuthServer;
struct IslandInfo;

// ─────────────────────────────────────────────────────────────────────────────
//  AuthSession — per-client state for the login server
// ─────────────────────────────────────────────────────────────────────────────

enum class LoginState {
    Handshake,      // Waiting for the login (HANDSHAKE_PAULA)
    Authenticated,  // Logged in — can select char server
    Closed          // Refused or disconnected
};

struct AccountRecord {
    uint32_t    account_id{ 0 };
    std::string username;
    uint8_t     gm_level{ 0 };
    bool        banned{ false };
    uint32_t    auth_token{ 0 };  // generated on login, stored in login_tokens DB
};

class AuthSession : public Session
{
public:
    AuthSession(asio::ip::tcp::socket socket, cAuthServer& srv)
        : Session(std::move(socket)), server_(srv) {}

    void on_packet(std::shared_ptr<Session> self, PacketBuffer pkt);
    void on_close (std::shared_ptr<Session> self);

    // A new connection: the Paula client speaks first (HANDSHAKE_PAULA), nothing is sent here
    void begin_handshake();

private:
    cAuthServer& server_;
    LoginState   state_{ LoginState::Handshake };
    AccountRecord account_;

    // Packet handlers
    void handle_handshake_paula(PacketBuffer& pkt);
    void handle_heartbeat      (PacketBuffer& pkt, Opcode op);

    // Paula (Season 2 / KTO) responses
    void send_login_response();          // 0x2CEE: CTrLoginUserAck, the island list
    void send_login_nak(uint32_t code);  // 0x2CEF: CTrLoginUserNak, then the connection closes
    void handle_server_select(PacketBuffer& pkt);   // 0x2CF1: CTrReserveWorldReq
    void send_reserve_world_nak(uint32_t code);     // 0x2CF3
    void send_channel_info(const IslandInfo& is);   // 0x2CF2: the picked island's LoginServer

    bool world_reserved_{ false };       // retail user +56: one reservation per login
};
