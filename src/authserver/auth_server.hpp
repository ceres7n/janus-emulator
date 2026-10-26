#pragma once
#include <asio.hpp>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include "session.hpp"
#include "database.hpp"
#include "config.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  cAuthServer
//
//  Responsible for:
//    • Accepting TCP connections from game clients (Paula protocol)
//    • Account login: the account name (no password in v0.1) against the accounts table
//    • Returning the island list (server select) and the picked island's LoginServer address
//    • Handing the login to the LoginServer through a one-time token (login_tokens)
// ─────────────────────────────────────────────────────────────────────────────

class AuthSession;

// One row of the retail FirstLoginSvr world table (uspfl_world_select_worldinfo, kept by sub_404D3C in
// a 50-slot table, one row per island). The pair (world, island) is the key CTrReserveWorldReq names.
struct IslandInfo
{
    uint16_t    world{ 1 };          // record +8
    uint16_t    island{ 1 };         // record +10 (IslandNo)
    std::string world_name;          // record +30, client text (CP949): the "Trickster <name>" window title
    std::string island_name;         // record +63, client text (CP949): the server-select button caption
    std::string ip;                  // record +12, that island's LoginServer, sent in 11506
    uint16_t    port{ 8001 };        // record +28
    bool        open{ true };        // record +4: a closed island is listed but refused (60008)
    bool        open_to_all{ true }; // record +100: 0 = listed only for the user flag (+764; here GM accounts)
};

class cAuthServer
{
public:
    explicit cAuthServer(asio::io_context& ioc);

    bool init(const Config& cfg);
    void run();
    void stop();

    Database& db() { return db_; }

    const std::string& char_ip()   const { return char_ip_; }
    uint16_t           char_port() const { return char_port_; }
    const std::vector<IslandInfo>& islands() const { return islands_; }

private:
    std::string char_ip_  { "127.0.0.1" };
    uint16_t    char_port_{ 8001 };
    std::vector<IslandInfo> islands_;

    void load_islands(const Config& cfg);
    asio::io_context&            ioc_;
    asio::ip::tcp::acceptor      acceptor_;
    asio::steady_timer           accept_retry_;     // re-arms the accept after an accept error
    Database                     db_;

    // Hardening (not retail): a connection that has not sent its login (HANDSHAKE_PAULA) by then is closed.
    static constexpr int kLoginTimeoutSec = 60;

    std::unordered_map<uint32_t, std::shared_ptr<AuthSession>> sessions_;
    std::mutex sessions_mutex_;
    uint32_t   next_session_id_{ 1 };

    void accept_next();
    void on_session_close(std::shared_ptr<Session> s);
};
