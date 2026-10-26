#pragma once
#include <asio.hpp>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <vector>
#include <cstdint>
#include "session.hpp"
#include "database.hpp"
#include "config.hpp"
#include "interserver/inter_link.hpp"
#include "interserver/inter_dispatch.hpp"

class ClientSession;

// cLoginServer — client-facing account login + char-select (mirrors the real
// cLoginServer). Serves the char list/create/delete/select flow (LCMD_*), persists
// via the shared DB, and hands the client off to a GameServer on select.
class cLoginServer
{
public:
    static constexpr int BASE_CHAR_SLOTS       = 4;
    static constexpr int MAX_CHARS_PER_ACCOUNT = 12;

    explicit cLoginServer(asio::io_context& ioc);

    bool init(const Config& cfg);
    void run();
    void stop();

    Database& db() { return db_; }   // auth / login_tokens only; char data lives on the Master

    const std::string& map_ip()   const { return map_ip_; }   // GameServer address sent on select
    uint16_t           map_port() const { return map_port_; }

    // Send a char request to the Master; `cb` fires (on the Master link strand) with the
    // reply reader positioned just after the echoed reqId. With no link, or when the link drops
    // before the reply, `cb` gets an empty reader instead (its reads fail: r.ok() is false).
    void master_request(uint16_t reqOpcode, const std::vector<uint8_t>& args,
                        std::function<void(paula::inter::StreamReader&)> cb);

private:
    void accept_next();
    void on_session_close(std::shared_ptr<Session> s);
    void connect_master();          // dial the MasterServer hub + handshake (SVR_LOGIN 3000)
    void retry_master();            // connect_master() again in kMasterRetrySec
    void on_master_lost();          // the link ended: fail the waiting requests, reconnect
    void handle_master_ack(paula::inter::StreamReader& r);  // reqId -> pending callback

    asio::io_context&       ioc_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer      accept_retry_;      // re-arms the accept after an accept error
    Database                db_;

    // Hardening (not retail): a connection that has not presented its login token (C2S 0x07D0) by then
    // is closed.
    static constexpr int kLoginTimeoutSec = 60;

    // Inter-server link to the MasterServer (persistence hub)
    paula::inter::Dispatcher                 master_disp_;
    std::shared_ptr<paula::inter::InterLink> master_link_;   // under pending_mtx_
    asio::steady_timer                       master_retry_;
    std::string master_ip_   { "127.0.0.1" };
    uint16_t    master_port_ { 9010 };
    uint32_t    next_req_id_ { 1 };
    std::mutex  pending_mtx_;                                 // master_link_, next_req_id_, pending_
    std::unordered_map<uint32_t, std::function<void(paula::inter::StreamReader&)>> pending_;

    static constexpr int kMasterRetrySec = 3;

    std::string map_ip_  { "127.0.0.1" };
    uint16_t    map_port_{ 8002 };

    std::unordered_map<uint32_t, std::shared_ptr<ClientSession>> sessions_;
    std::mutex sessions_mutex_;
    uint32_t   next_session_id_{ 1 };
};
