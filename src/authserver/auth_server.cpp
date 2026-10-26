#include "auth_server.hpp"
#include "auth_session.hpp"
#include "client_text.hpp"
#include "logger.hpp"
#include "paula_crypt.hpp"
#include "server_runtime.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>

cAuthServer::cAuthServer(asio::io_context& ioc)
    : ioc_(ioc)
    , acceptor_(ioc)
    , accept_retry_(ioc)
{}

// A table name as the client's text (CP949), cut to the 32 bytes a row's char[33] holds without
// splitting a double-byte character.
static std::string client_name(const std::string& utf8)
{
    std::string s = to_client_text(utf8);
    size_t n = 0;
    while (n < s.size()) {
        const size_t w = (static_cast<uint8_t>(s[n]) >= 0x81 && n + 1 < s.size()) ? 2 : 1;
        if (n + w > 32) break;
        n += w;
    }
    s.resize(n);
    return s;
}

// The retail FirstLoginSvr reads its world table from uspfl_world_select_worldinfo (sub_40223C; one
// row per island, kept in 50 slots by sub_404D3C, a repeated (world, island) pair overwriting its
// slot). That proc lives in the account DB (gmg_account), which the backup does not hold, so the rows
// come from the config instead:
//   island_N: world, island no, world name, island name, ip, port[, open[, open to all]]
// Without any island_N the list is the one island Eternal Paula had ("Ruby Island", the
// FirstLoginSvrW "World Activated" log lines) on char_server_ip:char_server_port.
void cAuthServer::load_islands(const Config& cfg)
{
    islands_.clear();
    for (int n = 1; n <= 50; ++n) {
        const std::string key = "island_" + std::to_string(n);
        if (!cfg.has(key)) continue;

        std::vector<std::string> f;
        const std::string v = cfg.get_str(key);
        for (size_t start = 0;;) {
            const size_t comma = v.find(',', start);
            const std::string part = v.substr(start, comma == std::string::npos ? std::string::npos
                                                                                 : comma - start);
            const size_t a = part.find_first_not_of(" \t");
            const size_t b = part.find_last_not_of(" \t");
            f.push_back(a == std::string::npos ? std::string() : part.substr(a, b - a + 1));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }

        IslandInfo is;
        try {
            if (f.size() < 6) throw std::invalid_argument("too few fields");
            is.world  = static_cast<uint16_t>(std::stoi(f[0]));
            is.island = static_cast<uint16_t>(std::stoi(f[1]));
            is.port   = static_cast<uint16_t>(std::stoi(f[5]));
        } catch (const std::exception&) {
            LOG_WARN("AuthServer: %s ignored (want: world, island no, world name, island name, ip, port"
                     "[, open[, open to all]])", key.c_str());
            continue;
        }
        if (is.world == 0) {    // world 0 marks an empty slot (sub_403A94 lists only +8 != 0)
            LOG_WARN("AuthServer: %s ignored (world 0)", key.c_str());
            continue;
        }
        is.world_name  = client_name(f[2]);
        is.island_name = client_name(f[3]);
        is.ip          = f[4].substr(0, 15);    // the 11506 IP is a char[16]
        if (f.size() > 6) is.open        = f[6] != "0";
        if (f.size() > 7) is.open_to_all = f[7] != "0";

        auto it = std::find_if(islands_.begin(), islands_.end(), [&](const IslandInfo& o) {
            return o.world == is.world && o.island == is.island;
        });
        if (it != islands_.end()) *it = is;
        else islands_.push_back(is);
    }

    if (islands_.empty()) {
        IslandInfo is;
        is.world_name  = client_name("Paula");
        is.island_name = client_name("Ruby Island");
        is.ip          = char_ip_;
        is.port        = char_port_;
        islands_.push_back(is);
    }

    for (const auto& is : islands_) {
        LOG_INFO("AuthServer: island world %u no %u '%s' -> %s:%u%s%s", is.world, is.island,
                 is.island_name.c_str(), is.ip.c_str(), is.port,
                 is.open ? "" : " (closed)", is.open_to_all ? "" : " (GM only)");
        asio::error_code aec;
        const asio::ip::address a = asio::ip::make_address(is.ip, aec);
        if (!aec && a.is_loopback())
            LOG_WARN("AuthServer: island world %u no %u sends players to %s (loopback): only a client on "
                     "this machine can follow it; give it this server's LAN/public address for others",
                     is.world, is.island, is.ip.c_str());
    }
    if (islands_.size() == 2)
        LOG_WARN("AuthServer: exactly 2 islands -> the client opens its two-world window, which "
                 "shows no island names");
    if (islands_.size() > 6)
        LOG_WARN("AuthServer: %zu islands, the client lists only 6 (the rest are not sent)",
                 islands_.size());
}

bool cAuthServer::init(const Config& cfg)
{
    // ── Database ──────────────────────────────────────────────────────────
    if (!db_.connect(
            cfg.get_str("db_host", "127.0.0.1"),
            cfg.get_str("db_user", "root"),
            cfg.get_str("db_pass", ""),
            cfg.get_str("db_name", "janus"),
            static_cast<uint16_t>(cfg.get_int("db_port", 3307))))
    {
        return false;
    }

    // ── Paula Protocol ───────────────────────────────────────────────────
    // The KR client speaks only the Paula protocol (the janus XOR login path is gone), so without the
    // S-box nobody could log in: refuse to start instead of listening in a mode no client speaks.
    if (!cfg.get_bool("paula_mode", true)) {
        LOG_FATAL("AuthServer: paula_mode false is not supported (the client speaks only the Paula protocol)");
        return false;
    }
    {
        std::string sbox_path = cfg.get_str("sbox_path", "tools/capture/sbox_cora.bin");
        if (!janus::PaulaCrypt::Instance().LoadSbox(sbox_path)) {
            LOG_FATAL("AuthServer: the Paula S-box failed to load from %s", sbox_path.c_str());
            return false;
        }
        LOG_INFO("Paula protocol support enabled (S-box loaded)");
    }

    // ── Char server address (the default island's LoginServer) ───────────
    char_ip_   = cfg.get_str("char_server_ip",   "127.0.0.1");
    char_port_ = static_cast<uint16_t>(cfg.get_int("char_server_port", 8001));

    // ── Server-select list (the retail world table) ──────────────────────
    load_islands(cfg);

    // ── Acceptor ──────────────────────────────────────────────────────────
    // bind_ip: the address players reach (0.0.0.0 = every interface).
    uint16_t port = static_cast<uint16_t>(cfg.get_int("port", 9980));
    asio::ip::address bind_ip;
    if (!paula::bind_address(cfg, "0.0.0.0", bind_ip)) return false;
    asio::ip::tcp::endpoint endpoint(bind_ip, port);
    if (!paula::open_acceptor(acceptor_, endpoint, "auth")) return false;

    LOG_INFO("AuthServer listening on %s:%u", bind_ip.to_string().c_str(), port);
    return true;
}

void cAuthServer::run()
{
    accept_next();
}

void cAuthServer::stop()
{
    asio::error_code ec;
    acceptor_.close(ec);
    accept_retry_.cancel();
}

void cAuthServer::accept_next()
{
    acceptor_.async_accept(
        [this](asio::error_code ec, asio::ip::tcp::socket socket) {
            if (ec) {
                if (ec == asio::error::operation_aborted) return;   // the acceptor was closed (stop)
                // Keep listening: an error such as running out of sockets/handles would otherwise end
                // the accepts for good. A short pause so a lasting error does not spin.
                LOG_ERROR("accept: %s (retrying)", ec.message().c_str());
                accept_retry_.expires_after(std::chrono::milliseconds(200));
                accept_retry_.async_wait([this](const asio::error_code& tec) { if (!tec) accept_next(); });
                return;
            }

            uint32_t id = next_session_id_++;
            auto sess = std::make_shared<AuthSession>(std::move(socket), *this);
            sess->set_session_id(id);
            sess->set_paula(janus::PaulaCrypt::Instance().IsLoaded());

            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                sessions_[id] = sess;
            }

            LOG_INFO("cAuthServer: new connection [%u] from %s",
                     id, sess->remote_address().c_str());

            // The handlers get the session as their argument: a lambda holding `sess` itself would make
            // the session own a reference to itself and never be freed.
            sess->start(
                [](std::shared_ptr<Session> s, PacketBuffer pkt) {
                    std::static_pointer_cast<AuthSession>(s)->on_packet(s, std::move(pkt));
                },
                [this](std::shared_ptr<Session> s) {
                    on_session_close(s);
                }
            );
            sess->arm_login_timeout(std::chrono::seconds(kLoginTimeoutSec));
            sess->begin_handshake();

            accept_next();
        });
}

void cAuthServer::on_session_close(std::shared_ptr<Session> s)
{
    std::static_pointer_cast<AuthSession>(s)->on_close(s);
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(s->session_id());
}
