#pragma once
#include <asio.hpp>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdint>
#include "config.hpp"
#include "database.hpp"
#include "store/char_store.hpp"
#include "interserver/inter_link.hpp"
#include "interserver/inter_dispatch.hpp"

// cMasterServer — the SQL brain + inter-server acceptor HUB (mirrors the real
// cMasterServer). Login/Game/System dial in; it owns the data stores (CharStore,
// …) and serves char/item/user/zone commands over the plaintext inter-server bus.
class cMasterServer
{
public:
    explicit cMasterServer(asio::io_context& ioc);

    bool init(const Config& cfg);
    void run();
    void stop();

private:
    void accept_next();
    void register_handlers();

    asio::io_context&       ioc_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer      accept_retry_;      // re-arms the accept after an accept error
    Database                db_;
    CharStore               char_store_{ db_ };
    paula::inter::Dispatcher disp_;
    std::vector<std::shared_ptr<paula::inter::InterLink>> peers_;   // the live links (under peers_mtx_)
    std::mutex              peers_mtx_;
    int                     next_peer_index_{ 0 };
};
