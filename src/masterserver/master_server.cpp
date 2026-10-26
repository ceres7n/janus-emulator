#include "master_server.hpp"
#include "logger.hpp"
#include "server_runtime.hpp"
#include "interserver/interserver_opcodes.hpp"
#include "store/char_record_io.hpp"
#include <algorithm>
#include <chrono>

using namespace paula::inter;

cMasterServer::cMasterServer(asio::io_context& ioc)
    : ioc_(ioc), acceptor_(ioc), accept_retry_(ioc) {}

bool cMasterServer::init(const Config& cfg)
{
    // MariaDB (same client lib as MySQL). Defaults match the local WAMP/MariaDB setup.
    if (!db_.connect(
            cfg.get_str("db_host", "127.0.0.1"),
            cfg.get_str("db_user", "root"),
            cfg.get_str("db_pass", ""),
            cfg.get_str("db_name", "janus"),
            static_cast<uint16_t>(cfg.get_int("db_port", 3307))))
        return false;

    register_handlers();

    // bind_ip defaults to 127.0.0.1: the hub serves char list / create / delete for any account to
    // whoever connects, with no authentication, so only the local Login/Game servers may reach it.
    // Widen it only for servers on other hosts, behind a firewall that admits just those hosts.
    uint16_t port = static_cast<uint16_t>(cfg.get_int("master_port", 9010));
    asio::ip::address bind_ip;
    if (!paula::bind_address(cfg, "127.0.0.1", bind_ip)) return false;
    asio::ip::tcp::endpoint ep(bind_ip, port);
    if (!paula::open_acceptor(acceptor_, ep, "master")) return false;
    if (!bind_ip.is_loopback())
        LOG_WARN("cMasterServer: bind_ip %s is not loopback - the hub has no authentication; firewall "
                 "port %u to the Login/Game hosts only", bind_ip.to_string().c_str(), port);

    LOG_INFO("cMasterServer hub listening on %s:%u", bind_ip.to_string().c_str(), port);
    return true;
}

void cMasterServer::register_handlers()
{
    // Peer handshake: a dialer (Login/Game/System) announces itself; we ack OK.
    disp_.on(SVR_LOGIN, [](InterLink& link, int peer, StreamReader& r) {
        std::string id = r.str();
        LOG_INFO("cMasterServer: peer %d SVR_LOGIN id='%s' -> ACK_OK", peer,
                 id.empty() ? "?" : id.c_str());
        link.send(SVR_LOGIN_ACK_OK);
    });

    // Char relay: Login (and later Game) ask the Master to run CharStore ops.
    // Request bodies start with a u32 reqId, echoed back in the reply for correlation. Every handler
    // answers, also when its store throws (a dropped DB connection), so the waiting LoginServer
    // session is never left hanging and the hub stays up.
    // CCMD_GET_BYOWNER (cUserStore::GetByOwnerFromDb): the account's slot count and characters.
    disp_.on(MREQ_CHAR_LIST, [this](InterLink& link, int, StreamReader& r) {
        uint32_t rid   = r.u32();
        uint32_t owner = r.u32();
        uint8_t  usable_slot = 0;
        std::vector<CharRecord> chars;
        try {
            chars = char_store_.LoadCharsByOwner(owner, usable_slot);
        } catch (const std::exception& e) {
            LOG_ERROR("CCMD_GET_BYOWNER failure: owner:%u: %s", owner, e.what());
            StreamWriter w; w.u32(rid);          // the failure answer: the reqId alone
            link.send(MACK_CHAR_LIST, w.data());
            return;
        }
        StreamWriter w; w.u32(rid); w.u8(usable_slot); w.u8(static_cast<uint8_t>(chars.size()));
        for (auto& c : chars) paula::pack_char(w, c);
        link.send(MACK_CHAR_LIST, w.data());
    });

    // CCMD_NEW (sub_407520): 3630 ACK_OK with the new character's entry, or 3629 ACK_CANCEL with
    // InsertIntoDb's result code.
    disp_.on(MREQ_CHAR_CREATE, [this](InterLink& link, int, StreamReader& r) {
        uint32_t   rid = r.u32();
        CharRecord c   = paula::unpack_char(r);
        int code = 9996;
        try {
            code = char_store_.MakeNewCharacter(c);
        } catch (const std::exception& e) {
            LOG_ERROR("cCharStore::InsertIntoDb failure: %s", e.what());
        }
        if (code) LOG_INFO("CCMD_NEW_ACK_CANCEL:owner:%u (%d)", c.owner, code);
        else      LOG_INFO("CCMD_NEW_ACK_OK:owner:%u, name:%s", c.owner, c.name);
        StreamWriter w; w.u32(rid); w.u32(static_cast<uint32_t>(code));
        if (!code) paula::pack_char(w, c);
        link.send(MACK_CHAR_CREATE, w.data());
    });

    // CCMD_DEL (3631): uspm_deleted_char_insert (uid, the client's IP).
    disp_.on(MREQ_CHAR_DELETE, [this](InterLink& link, int, StreamReader& r) {
        uint32_t rid   = r.u32();
        uint32_t owner = r.u32();
        uint32_t uid   = r.u32();
        std::string ip = r.str();
        int code = 9996;
        try {
            code = char_store_.DeleteFromDb(uid, owner, ip);
        } catch (const std::exception& e) {
            LOG_ERROR("uspm_deleted_char_insert failure: %s", e.what());
        }
        LOG_INFO("CCMD_DEL_ACK_%s:owner:%u, chr:%u (%d)", code ? "CANCEL" : "OK", owner, uid, code);
        StreamWriter w; w.u32(rid); w.u8(code ? 0 : 1);
        link.send(MACK_CHAR_DELETE, w.data());
    });
}

void cMasterServer::run() { accept_next(); }

void cMasterServer::stop()
{
    asio::error_code ec;
    acceptor_.close(ec);
    accept_retry_.cancel();
}

void cMasterServer::accept_next()
{
    acceptor_.async_accept(
        [this](asio::error_code ec, asio::ip::tcp::socket sock) {
            if (ec) {
                if (ec == asio::error::operation_aborted) return;   // the acceptor was closed (stop)
                // Keep listening: an error such as running out of sockets/handles would otherwise end
                // the accepts for good. A short pause so a lasting error does not spin.
                LOG_ERROR("master accept: %s (retrying)", ec.message().c_str());
                accept_retry_.expires_after(std::chrono::milliseconds(200));
                accept_retry_.async_wait([this](const asio::error_code& tec) { if (!tec) accept_next(); });
                return;
            }
            int idx = next_peer_index_++;
            auto link = std::make_shared<InterLink>(std::move(sock), disp_, idx);
            link->set_on_close([this, idx]() {
                LOG_INFO("cMasterServer: peer %d disconnected", idx);
                std::lock_guard<std::mutex> lk(peers_mtx_);
                peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                            [idx](const std::shared_ptr<InterLink>& p) {
                                                return p->peer_index() == idx; }),
                             peers_.end());
            });
            {
                std::lock_guard<std::mutex> lk(peers_mtx_);
                peers_.push_back(link);
            }
            LOG_INFO("cMasterServer: peer %d connected", idx);
            link->start();
            accept_next();
        });
}
