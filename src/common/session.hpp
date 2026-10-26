#pragma once
#include <asio.hpp>
#include <memory>
#include <functional>
#include <utility>
#include <array>
#include "packet.hpp"
#include "logger.hpp"
#include "paula_crypt.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  Session — async TCP session using standalone ASIO
//
//  Each connected client gets one Session. Reads incoming packets, dispatches
//  them via the on_packet callback. Thread-safe send() via strand.
//
//  Trickster packet framing (Paula, the KR client): a 9-byte header — [0..1] length (u16 LE, whole
//  packet), [2..3] opcode, [4..5] block id, [6] random key, [7] packing, [8] checksum — the body,
//  then a 2-byte tail (body checksum); header and body are ciphered with the S-box (PaulaCrypt).
//  The legacy framing (paula_mode off) is [0..1] opcode, [2..3] length incl. this 4-byte header.
// ─────────────────────────────────────────────────────────────────────────────

class Session : public std::enable_shared_from_this<Session>
{
public:
    using PacketHandler = std::function<void(std::shared_ptr<Session>, PacketBuffer)>;
    using CloseHandler  = std::function<void(std::shared_ptr<Session>)>;

    explicit Session(asio::ip::tcp::socket socket)
        : socket_(std::move(socket))
        , strand_(asio::make_strand(socket_.get_executor()))
        , login_timer_(socket_.get_executor())
    {
        // Disable Nagle's algorithm. Without this, a small S2C packet (a warp
        // RELOCATE, a position correction) gets held in the kernel send buffer
        // until the client ACKs — and the client's delayed-ACK only fires when it
        // has its own data to send (i.e. when the player clicks/moves). The result
        // is "the packet is only delivered when I do something": the exact symptom.
        // Game traffic is many small latency-sensitive packets, so Nagle must be off.
        asio::error_code ec;
        socket_.set_option(asio::ip::tcp::no_delay(true), ec);
        if (ec) LOG_WARN("Session: set TCP_NODELAY failed: %s", ec.message().c_str());
    }

    virtual ~Session() = default;

    void start(PacketHandler on_packet, CloseHandler on_close)
    {
        on_packet_ = std::move(on_packet);
        on_close_  = std::move(on_close);
        read_header();
    }

    // Thread-safe send
    void send(PacketBuffer pkt)
    {
        // The frame's length field is a u16: a bigger packet cannot be framed (and the Paula cipher would
        // run over a wrapped length). Drop it rather than corrupt the stream.
        const size_t framed = pkt.size() + ((is_paula_ && pkt.is_paula()) ? PacketBuffer::PAULA_TAIL_SIZE : 0);
        if (framed > PacketBuffer::MAX_PACKET) {
            LOG_ERROR("Session %u: S2C 0x%04X is %zu bytes, over the %zu-byte frame limit; dropped",
                      session_id_, static_cast<unsigned>(pkt.opcode()), framed, PacketBuffer::MAX_PACKET);
            return;
        }

        if (is_paula_ && pkt.is_paula()) {
            // Allocate space for the 2-byte Season 2 CRC tail
            pkt.write<uint16_t>(0);
            pkt.finalise(); // Write total length to index 0

            uint8_t* data = const_cast<uint8_t*>(pkt.data());
            uint16_t length = static_cast<uint16_t>(pkt.size());
            uint16_t opcode = static_cast<uint16_t>(pkt.opcode());

            // Header word @4 = the command-block id (client sub_82A050/sub_6EB350). 0 for an
            // ordinary packet (dispatched immediately); a shared non-zero id groups the packets of
            // one action (attack/skill/item) that the client runs on the matching CLOSE (0x079F).
            // See PacketBuffer::block_id. (This replaces the old incrementing server_sequence_ that
            // wrongly tagged every packet as its own never-closed block.)
            uint16_t blk = pkt.block_id();
            std::memcpy(data + 4, &blk, 2);

            janus::PaulaCrypt::Instance().EncryptHeader(data, paula_send_key_);
            janus::PaulaCrypt::Instance().EncryptData(data, length, opcode, paula_send_key_);
            // S2C key (paula_send_key_) is static — client's network_object[9] never advances.
            // Do NOT sync current_paula_key_ here; C2S key is independent and tracked via UnpackData.
        } else {
            pkt.finalise();
        }

        auto buf = std::make_shared<std::vector<uint8_t>>(pkt.data(), pkt.data() + pkt.size());
        asio::post(strand_, [this, self = shared_from_this(), buf]() {
            if (close_when_flushed_ || !socket_.is_open()) return;   // closing: nothing more goes out
            // Hardening (not retail): a peer that stops reading would grow this queue without bound
            // (every broadcast adds to it); past kMaxQueuedBytes the connection is dropped instead.
            if (queued_bytes_ + buf->size() > kMaxQueuedBytes) {
                LOG_WARN("Session %u: peer is not reading (%zu bytes queued); closing", session_id_,
                         queued_bytes_);
                shutdown_and_close();
                return;
            }
            queued_bytes_ += buf->size();
            bool writing = !write_queue_.empty();
            write_queue_.push_back(buf);
            if (!writing) do_write();
        });
    }

    void close()
    {
        asio::post(strand_, [this, self = shared_from_this()]() { shutdown_and_close(); });
    }

    // Closes the connection once every packet queued so far has been written: for a refusal the client
    // must still receive (S2C PAULA_LOGIN_FAIL). Packets sent after this are dropped.
    void close_after_flush()
    {
        asio::post(strand_, [this, self = shared_from_this()]() {
            close_when_flushed_ = true;
            if (write_queue_.empty()) shutdown_and_close();
        });
    }

    // Hardening (not retail): a connection that has not finished its first login step within `limit`
    // is closed (an idle or half-open connection would otherwise hold its socket forever). The
    // session calls login_done() once that step has succeeded.
    void arm_login_timeout(std::chrono::seconds limit)
    {
        asio::post(strand_, [this, self = shared_from_this(), limit]() {
            if (login_done_ || !socket_.is_open()) return;
            login_timer_.expires_after(limit);
            login_timer_.async_wait(asio::bind_executor(strand_,
                [this, self](const asio::error_code& ec) {
                    if (ec || login_done_ || !socket_.is_open()) return;
                    LOG_INFO("Session %u: no login within the time limit (%s); closing", session_id_,
                             remote_address().c_str());
                    shutdown_and_close();
                }));
        });
    }

    // Called on the session strand (from on_packet) when the first login step has succeeded.
    void login_done()
    {
        login_done_ = true;
        login_timer_.cancel();
    }

    std::string remote_address() const
    {
        try {
            return socket_.remote_endpoint().address().to_string();
        } catch (...) { return "?"; }
    }

    uint32_t session_id() const { return session_id_; }
    void set_session_id(uint32_t id) { session_id_ = id; }

    // Encryption key (XOR, set after SESSION_ENCRYPT exchange)
    void set_crypt_key(uint32_t key) { crypt_key_ = key; }
    uint32_t crypt_key() const { return crypt_key_; }

    void set_paula(bool p) { is_paula_ = p; }
    bool is_paula() const { return is_paula_; }



    // Run a callback on the session strand (serialized with the session's own handlers).
    void run_on_strand(std::function<void()> fn)
    {
        asio::post(strand_, [self = shared_from_this(), fn = std::move(fn)]() { fn(); });
    }

    // Schedule a one-shot callback on the session strand after `ms` milliseconds.
    // Safe to call from any context; the timer is kept alive by a shared_ptr.
    void schedule_delayed(std::chrono::milliseconds ms, std::function<void()> fn)
    {
        auto timer = std::make_shared<asio::steady_timer>(socket_.get_executor());
        timer->expires_after(ms);
        timer->async_wait(
            asio::bind_executor(strand_,
                [fn = std::move(fn), timer](const asio::error_code& ec) {
                    if (!ec) fn();
                }));
    }

protected:
    asio::ip::tcp::socket socket_;
    asio::strand<asio::any_io_executor> strand_;
    bool is_paula_{ false };

private:
    PacketHandler on_packet_;
    CloseHandler  on_close_;
    std::array<uint8_t, PacketBuffer::PAULA_HEADER_SIZE> header_buf_{}; // Larger to accommodate Paula
    std::vector<uint8_t> body_buf_;
    uint8_t  current_paula_index_{ 0 };
    uint8_t  current_paula_key_  { 0 };   // C2S recv key — tracks client network_object[0]; advances on each C2S recv
    uint8_t  paula_send_key_     { 0 };   // S2C send key — tracks client network_object[9]; static (never advances)
    uint16_t paula_logical_len_  { 0 };   // logical packet length after stripping dummy bytes (used across read_header/read_body)
    bool     is_first_packet_    { true };
    size_t   current_header_size_{ 4 };
    std::vector<std::shared_ptr<std::vector<uint8_t>>> write_queue_;
    size_t   queued_bytes_       { 0 };   // bytes in write_queue_ (strand only)
    bool     close_when_flushed_ { false };
    bool     login_done_         { false };
    asio::steady_timer login_timer_;
    uint32_t session_id_{ 0 };
    uint32_t crypt_key_ { 0 };

    static constexpr size_t kMaxQueuedBytes = 2 * 1024 * 1024;

    void shutdown_and_close()
    {
        asio::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

    void read_header()
    {
        auto self = shared_from_this();
        size_t initial_read_size = is_paula_ ? 9 : 4;

        asio::async_read(socket_, asio::buffer(header_buf_.data(), initial_read_size),
            asio::bind_executor(strand_, [this, self, initial_read_size](asio::error_code ec, size_t n) {
                if (ec) { handle_error(ec); return; }

                if (is_paula_) {
                    current_header_size_ = 9;

                    // Decrypt the 9-byte header in-place.
                    if (Logger::instance().enabled(LogLevel::Debug)) {
                        std::string hex;
                        for (int i = 0; i < 9; ++i) { char b[4]; snprintf(b, sizeof(b), "%02X ", header_buf_[i]); hex += b; }
                        LOG_DEBUG("Session %u: Paula hdr key=%02X first=%d bytes: %s",
                                  session_id_, current_paula_key_, (int)is_first_packet_, hex.c_str());
                    }
                    // Save before UnpackHeader clears it: used to detect K_init from first packet.
                    bool was_first_packet = is_first_packet_;
                    if (!janus::PaulaCrypt::Instance().UnpackHeader(
                            header_buf_.data(), current_paula_key_, is_first_packet_)) {
                        LOG_WARN("Session %u: UnpackHeader failed (key=%02X first=%d), closing",
                                 session_id_, current_paula_key_, (int)is_first_packet_);
                        handle_close();
                        return;
                    }
                    // One-time S2C key init: network_object[9] (client S2C verify key) starts at
                    // the same K_init as network_object[0] (C2S key).  We learn K_init from the
                    // first C2S packet (possibly via brute-force heal).  After this point
                    // paula_send_key_ stays constant — the client never advances network_object[9].
                    if (was_first_packet) {
                        paula_send_key_ = current_paula_key_;
                        LOG_DEBUG("Session %u: S2C key initialised to K_init=0x%02X",
                                  session_id_, paula_send_key_);
                    }

                    // Read the decoded wire length (includes any dummy bytes appended by client).
                    uint16_t wire_total_len;
                    std::memcpy(&wire_total_len, header_buf_.data(), 2);

                    // ExcludeDummy: reduces total_len by (randKey % 13) dummy bytes if packing bit3 set.
                    // This gives the logical length; wire_total_len is the actual byte count on the wire.
                    uint16_t total_len = wire_total_len;
                    if (!janus::PaulaCrypt::Instance().ExcludeDummy(header_buf_.data(), total_len)) {
                        LOG_WARN("Session %u: ExcludeDummy failed (len=%u)", session_id_, total_len);
                        handle_close();
                        return;
                    }

                    // The logical length can only shrink the wire one (the dummies come off the end):
                    // anything else would make read_body decrypt past the bytes actually read.
                    if (total_len < 11 || total_len > wire_total_len) {
                        LOG_WARN("Session %u: invalid Paula decrypted length %u (wire %u)", session_id_,
                                 total_len, wire_total_len);
                        handle_close();
                        return;
                    }

                    // Save logical length for use in read_body (UnpackData + trim).
                    paula_logical_len_ = total_len;

                    // IMPORTANT: read the full WIRE body (including dummy bytes) so the TCP stream
                    // stays framed.  Using logical_len here would leave dummy bytes in the socket
                    // buffer and corrupt all subsequent packet reads.
                    size_t wire_body_len = static_cast<size_t>(wire_total_len) - 9;
                    body_buf_.resize(wire_body_len);
                    read_body(wire_body_len, wire_total_len);
                    return;
                }

                // Standard 4-byte header
                uint16_t total_len;
                std::memcpy(&total_len, header_buf_.data() + 2, 2);
                
                if (total_len < 4 || total_len > PacketBuffer::MAX_PACKET) {
                    LOG_WARN("Session %u: invalid std header length %u", session_id_, total_len);
                    handle_close();
                    return;
                }

                size_t body_len = total_len - 4;
                body_buf_.resize(body_len);
                current_header_size_ = 4;
                read_body(body_len, total_len);
            }));
    }

    void read_body(size_t body_len, uint16_t total_len)
    {
        auto self = shared_from_this();
        if (body_len == 0) {
            dispatch_packet();
            return;
        }
        asio::async_read(socket_, asio::buffer(body_buf_),
            asio::bind_executor(strand_, [this, self, total_len](asio::error_code ec, size_t) {
                if (ec) { handle_error(ec); return; }
                
                if (is_paula_) {
                    // full_packet is wire-sized (header + wire body) to hold all bytes including dummies.
                    std::vector<uint8_t> full_packet(9 + body_buf_.size());
                    std::memcpy(full_packet.data(), header_buf_.data(), 9);
                    std::memcpy(full_packet.data() + 9, body_buf_.data(), body_buf_.size());

                    // UnpackData processes only the logical portion (no dummies), and advances
                    // current_paula_key_ after successful decryption (mirrors client sub_AB16B0).
                    if (!janus::PaulaCrypt::Instance().UnpackData(full_packet.data(), full_packet.size(),
                                                                  paula_logical_len_, current_paula_key_)) {
                        LOG_WARN("Session %u: UnpackData checksum mismatch", session_id_);
                        handle_close();
                        return;
                    }
                    LOG_DEBUG("Session %u: UnpackData ok, c2s_key=%02X",
                              session_id_, current_paula_key_);
                    // Copy back only the logical payload (no dummies, no CRC tail).
                    size_t logical_payload = static_cast<size_t>(paula_logical_len_) - 9 - 2;
                    std::memcpy(body_buf_.data(), full_packet.data() + 9, logical_payload);
                    body_buf_.resize(logical_payload);
                }
                
                dispatch_packet();
            }));
    }

    void dispatch_packet()
    {
        // Assemble raw bytes: header + body
        std::vector<uint8_t> raw(current_header_size_ + body_buf_.size());
        std::memcpy(raw.data(), header_buf_.data(), current_header_size_);
        if (!body_buf_.empty())
            std::memcpy(raw.data() + current_header_size_, body_buf_.data(), body_buf_.size());

        try {
            PacketBuffer pkt(raw.data(), raw.size(), is_paula_, current_header_size_);
            if (on_packet_)
                on_packet_(shared_from_this(), std::move(pkt));
        } catch (const std::exception& e) {
            LOG_WARN("Session %u: packet parse error: %s", session_id_, e.what());
        }

        // Continue reading
        read_header();
    }

    void do_write()
    {
        auto& buf = write_queue_.front();
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(*buf),
            asio::bind_executor(strand_, [this, self](asio::error_code ec, size_t) {
                if (ec) { handle_error(ec); return; }
                queued_bytes_ -= write_queue_.front()->size();
                write_queue_.erase(write_queue_.begin());
                if (!write_queue_.empty()) do_write();
                else if (close_when_flushed_) shutdown_and_close();
            }));
    }

    void handle_error(const asio::error_code& ec)
    {
        if (ec != asio::error::operation_aborted &&
            ec != asio::error::eof)
        {
            LOG_WARN("Session %u: %s", session_id_, ec.message().c_str());
        }
        handle_close();
    }

    void handle_close()
    {
        asio::error_code ec;
        socket_.close(ec);
        login_timer_.cancel();
        // Notify once (a failed read and a failed write both end here), then drop both handlers: a
        // handler that holds a shared_ptr to this session would otherwise keep it alive forever.
        CloseHandler on_close = std::move(on_close_);
        on_close_  = nullptr;
        if (on_close) on_close(shared_from_this());
        on_packet_ = nullptr;
    }
};
