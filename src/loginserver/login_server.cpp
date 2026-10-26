#include "login_server.hpp"
#include "client_session.hpp"
#include "logger.hpp"
#include "paula_crypt.hpp"
#include "server_runtime.hpp"
#include "interserver/interserver_opcodes.hpp"
#include <chrono>

cLoginServer::cLoginServer(asio::io_context& ioc)
    : ioc_(ioc), acceptor_(ioc), accept_retry_(ioc), master_retry_(ioc) {}

bool cLoginServer::init(const Config& cfg)
{
    // MariaDB (same client lib as MySQL). Defaults match the local WAMP/MariaDB setup.
    if (!db_.connect(
            cfg.get_str("db_host", "127.0.0.1"),
            cfg.get_str("db_user", "root"),
            cfg.get_str("db_pass", ""),
            cfg.get_str("db_name", "janus"),
            static_cast<uint16_t>(cfg.get_int("db_port", 3307))))
        return false;

    // Client packet cipher (Paula protocol). Without the S-box no client could connect: fatal.
    if (cfg.get_bool("paula_mode", true)) {
        std::string sbox_path = cfg.get_str("sbox_path", "tools/capture/sbox_cora.bin");
        if (!janus::PaulaCrypt::Instance().LoadSbox(sbox_path)) {
            LOG_FATAL("LoginServer: paula_mode on but S-box failed to load from %s", sbox_path.c_str());
            return false;
        }
        LOG_INFO("LoginServer: Paula protocol enabled (S-box loaded)");
    }

    // GameServer address handed to the client on char select
    map_ip_   = cfg.get_str("game_server_ip",   "127.0.0.1");
    map_port_ = static_cast<uint16_t>(cfg.get_int("game_server_port", 8002));
    {
        asio::error_code aec;
        const asio::ip::address a = asio::ip::make_address(map_ip_, aec);
        if (!aec && a.is_loopback())
            LOG_WARN("LoginServer: game_server_ip %s is loopback: only a client on this machine can reach "
                     "the GameServer; set this server's LAN/public address for others", map_ip_.c_str());
    }

    // bind_ip: the address players reach (0.0.0.0 = every interface).
    uint16_t port = static_cast<uint16_t>(cfg.get_int("port", 8001));
    asio::ip::address bind_ip;
    if (!paula::bind_address(cfg, "0.0.0.0", bind_ip)) return false;
    asio::ip::tcp::endpoint ep(bind_ip, port);
    if (!paula::open_acceptor(acceptor_, ep, "login")) return false;

    LOG_INFO("cLoginServer listening on %s:%u (game -> %s:%u)", bind_ip.to_string().c_str(), port,
             map_ip_.c_str(), map_port_);

    // MasterServer hub link (persistence relay)
    master_ip_   = cfg.get_str("master_ip", "127.0.0.1");
    master_port_ = static_cast<uint16_t>(cfg.get_int("master_port", 9010));
    master_disp_.on(paula::inter::SVR_LOGIN_ACK_OK, [](paula::inter::InterLink&, int, paula::inter::StreamReader&) {
        LOG_INFO("cLoginServer: Master handshake OK (link up)");
    });
    master_disp_.on(paula::inter::SVR_LOGIN_ACK_CANCEL, [](paula::inter::InterLink&, int, paula::inter::StreamReader&) {
        LOG_ERROR("cLoginServer: Master rejected handshake");
    });
    // Char-relay replies -> correlate back to the waiting session callback.
    auto ack = [this](paula::inter::InterLink&, int, paula::inter::StreamReader& r) { handle_master_ack(r); };
    master_disp_.on(paula::inter::MACK_CHAR_LIST,   ack);
    master_disp_.on(paula::inter::MACK_CHAR_CREATE, ack);
    master_disp_.on(paula::inter::MACK_CHAR_DELETE, ack);
    connect_master();

    return true;
}

void cLoginServer::master_request(uint16_t reqOpcode, const std::vector<uint8_t>& args,
                                  std::function<void(paula::inter::StreamReader&)> cb)
{
    uint32_t rid = 0;
    std::shared_ptr<paula::inter::InterLink> link;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        link = master_link_;
        if (link) {
            rid = next_req_id_++;
            pending_[rid] = std::move(cb);
        }
    }
    if (!link) {
        // No Master: answer at once with an empty reply, which every caller takes as its failure
        // (instead of leaving the client waiting at char select for an answer that never comes).
        LOG_ERROR("cLoginServer: no Master link; request 0x%X refused", reqOpcode);
        const std::vector<uint8_t> none;
        paula::inter::StreamReader r(none);
        cb(r);
        return;
    }
    paula::inter::StreamWriter w;
    w.u32(rid);
    w.raw(args);
    link->send(reqOpcode, w.data());
}

void cLoginServer::handle_master_ack(paula::inter::StreamReader& r)
{
    uint32_t rid = r.u32();
    std::function<void(paula::inter::StreamReader&)> cb;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        auto it = pending_.find(rid);
        if (it != pending_.end()) { cb = std::move(it->second); pending_.erase(it); }
    }
    if (cb) cb(r);
    else LOG_WARN("cLoginServer: master ack with unknown reqId %u", rid);
}

void cLoginServer::connect_master()
{
    auto sock = std::make_shared<asio::ip::tcp::socket>(ioc_);
    asio::error_code aec;
    auto addr = asio::ip::make_address(master_ip_, aec);
    if (aec) { LOG_ERROR("cLoginServer: bad master_ip '%s'", master_ip_.c_str()); return; }
    asio::ip::tcp::endpoint ep(addr, master_port_);

    sock->async_connect(ep, [this, sock](std::error_code ec) {
        if (ec) {
            LOG_ERROR("cLoginServer: connect to Master %s:%u failed: %s (start MasterServer first; "
                      "retrying every %d s)", master_ip_.c_str(), master_port_, ec.message().c_str(),
                      kMasterRetrySec);
            retry_master();
            return;
        }
        auto link = std::make_shared<paula::inter::InterLink>(std::move(*sock), master_disp_, 0);
        link->set_on_close([this]() { on_master_lost(); });
        {
            std::lock_guard<std::mutex> lk(pending_mtx_);
            master_link_ = link;
        }
        link->start();
        paula::inter::StreamWriter w;
        w.str("LoginServer");
        link->send(paula::inter::SVR_LOGIN, w.data());
        LOG_INFO("cLoginServer: connected to Master %s:%u, sent SVR_LOGIN", master_ip_.c_str(), master_port_);
    });
}

void cLoginServer::retry_master()
{
    master_retry_.expires_after(std::chrono::seconds(kMasterRetrySec));
    master_retry_.async_wait([this](const asio::error_code& ec) { if (!ec) connect_master(); });
}

// The Master link ended (the MasterServer stopped or restarted): every request still waiting gets the
// empty failure answer, and the link is dialled again.
void cLoginServer::on_master_lost()
{
    std::unordered_map<uint32_t, std::function<void(paula::inter::StreamReader&)>> waiting;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        master_link_.reset();
        waiting.swap(pending_);
    }
    LOG_ERROR("cLoginServer: Master link lost (%zu request(s) failed); reconnecting", waiting.size());
    const std::vector<uint8_t> none;
    for (auto& kv : waiting) {
        paula::inter::StreamReader r(none);
        if (kv.second) kv.second(r);
    }
    retry_master();
}

void cLoginServer::run() { accept_next(); }

void cLoginServer::stop()
{
    asio::error_code ec;
    acceptor_.close(ec);
    accept_retry_.cancel();
    master_retry_.cancel();
}

void cLoginServer::accept_next()
{
    acceptor_.async_accept(
        [this](asio::error_code ec, asio::ip::tcp::socket socket) {
            if (ec) {
                if (ec == asio::error::operation_aborted) return;   // the acceptor was closed (stop)
                // Keep listening: an error such as running out of sockets/handles would otherwise end
                // the accepts for good. A short pause so a lasting error does not spin.
                LOG_ERROR("login accept: %s (retrying)", ec.message().c_str());
                accept_retry_.expires_after(std::chrono::milliseconds(200));
                accept_retry_.async_wait([this](const asio::error_code& tec) { if (!tec) accept_next(); });
                return;
            }

            uint32_t id = next_session_id_++;
            auto sess = std::make_shared<ClientSession>(std::move(socket), *this);
            sess->set_session_id(id);
            sess->set_paula(janus::PaulaCrypt::Instance().IsLoaded());
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                sessions_[id] = sess;
            }
            LOG_INFO("cLoginServer: connection [%u] from %s", id, sess->remote_address().c_str());

            // The handlers get the session as their argument: a lambda holding `sess` itself would make
            // the session own a reference to itself and never be freed.
            sess->start(
                [](std::shared_ptr<Session> s, PacketBuffer pkt) {
                    std::static_pointer_cast<ClientSession>(s)->on_packet(s, std::move(pkt));
                },
                [this](std::shared_ptr<Session> s) { on_session_close(s); }
            );
            sess->arm_login_timeout(std::chrono::seconds(kLoginTimeoutSec));
            sess->begin_handshake();
            accept_next();
        });
}

void cLoginServer::on_session_close(std::shared_ptr<Session> s)
{
    std::static_pointer_cast<ClientSession>(s)->on_close(s);
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(s->session_id());
}
