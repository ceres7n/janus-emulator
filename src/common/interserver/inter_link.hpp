#pragma once
#include <asio.hpp>
#include <functional>
#include <memory>
#include <deque>
#include <vector>
#include <cstdint>
#include "inter_frame.hpp"
#include "inter_dispatch.hpp"

// One peer TCP connection on the inter-server bus: accumulates bytes (128 KB),
// extracts plaintext frames, dispatches by opcode. See spec 03 §"Send/receive".
namespace paula::inter {

class InterLink : public std::enable_shared_from_this<InterLink> {
public:
    InterLink(asio::ip::tcp::socket sock, Dispatcher& disp, int peerIndex);

    void start();                                    // begin async read loop
    void send(uint16_t opcode, const std::vector<uint8_t>& body, uint16_t field2 = 0);
    void send(uint16_t opcode) { send(opcode, empty_, 0); }
    void close();

    // Called once, on the link's strand, when the link ends (read error, peer gone, corrupt frame).
    // Set it before start().
    void set_on_close(std::function<void()> fn) { on_close_ = std::move(fn); }

    int  peer_index() const { return peerIndex_; }
    void set_peer_index(int i) { peerIndex_ = i; }

private:
    void do_read();
    void process();
    void do_write();
    void link_down();                                // close + on_close_ (once)

    std::function<void()> on_close_;

    asio::ip::tcp::socket socket_;
    asio::strand<asio::any_io_executor> strand_;
    Dispatcher&           disp_;
    int                   peerIndex_;
    std::vector<uint8_t>  accum_;
    uint8_t               rbuf_[8192];
    std::deque<std::vector<uint8_t>> wq_;
    bool                  writing_ = false;
    std::vector<uint8_t>  empty_;

    static constexpr std::size_t kMaxAccum = 0x20000;   // 128 KB
};

} // namespace paula::inter
