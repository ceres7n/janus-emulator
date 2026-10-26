#include "inter_link.hpp"
#include "logger.hpp"

namespace paula::inter {

InterLink::InterLink(asio::ip::tcp::socket sock, Dispatcher& disp, int peerIndex)
    : socket_(std::move(sock)), strand_(asio::make_strand(socket_.get_executor())),
      disp_(disp), peerIndex_(peerIndex)
{
    accum_.reserve(4096);
}

void InterLink::start() { do_read(); }

void InterLink::close()
{
    asio::error_code ec;
    socket_.close(ec);
}

void InterLink::link_down()
{
    close();
    auto fn = std::move(on_close_);
    on_close_ = nullptr;
    if (fn) fn();
}

void InterLink::do_read()
{
    auto self = shared_from_this();
    socket_.async_read_some(asio::buffer(rbuf_, sizeof(rbuf_)),
        asio::bind_executor(strand_, [this, self](std::error_code ec, std::size_t n) {
            if (ec) {
                LOG_WARN("InterLink peer %d read error: %s", peerIndex_, ec.message().c_str());
                link_down();
                return;
            }
            if (accum_.size() + n > kMaxAccum) {
                LOG_WARN("InterLink peer %d accumulate overflow (%zu); dropping",
                         peerIndex_, accum_.size() + n);
                link_down();
                return;
            }
            accum_.insert(accum_.end(), rbuf_, rbuf_ + n);
            process();
            do_read();
        }));
}

void InterLink::process()
{
    std::size_t off = 0;
    while (true) {
        Frame f;
        int r = try_decode(accum_.data() + off, accum_.size() - off, f);
        if (r == 0) break;                       // need more bytes
        if (r < 0) {
            LOG_WARN("InterLink peer %d: corrupt frame; dropping link", peerIndex_);
            accum_.clear();
            close();
            return;
        }
        StreamReader rd(f.body);
        // A handler that throws (a store's DBException, ...) loses only its own frame: escaping here it
        // would leave the io_context's run() and end the server.
        try {
            if (!disp_.dispatch(*this, peerIndex_, f.opcode, rd))
                LOG_WARN("InterLink peer %d: no handler for opcode %u", peerIndex_, f.opcode);
        } catch (const std::exception& e) {
            LOG_ERROR("InterLink peer %d: handler for opcode %u threw: %s", peerIndex_, f.opcode, e.what());
        } catch (...) {
            LOG_ERROR("InterLink peer %d: handler for opcode %u threw", peerIndex_, f.opcode);
        }
        off += static_cast<std::size_t>(r);
    }
    if (off) accum_.erase(accum_.begin(), accum_.begin() + off);
}

void InterLink::send(uint16_t opcode, const std::vector<uint8_t>& body, uint16_t field2)
{
    auto frame = encode(opcode, body, field2);
    auto self = shared_from_this();
    asio::post(strand_,
        [this, self, frame = std::move(frame)]() mutable {
            wq_.push_back(std::move(frame));
            if (!writing_) do_write();
        });
}

void InterLink::do_write()
{
    if (wq_.empty()) { writing_ = false; return; }
    writing_ = true;
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(wq_.front()),
        asio::bind_executor(strand_, [this, self](std::error_code ec, std::size_t) {
            if (ec) {
                LOG_WARN("InterLink peer %d write error: %s", peerIndex_, ec.message().c_str());
                writing_ = false;
                return;
            }
            wq_.pop_front();
            do_write();
        }));
}

} // namespace paula::inter
