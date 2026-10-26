#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "opcodes.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  PacketBuffer — read/write byte buffer for Trickster packets
//
//  Wire format (little-endian). Paula (is_paula, the KR client): a 9-byte header — [0..1] length
//  (whole packet), [2..3] opcode, [4..5] block id, [6..8] key / packing / checksum — then the payload;
//  Session::send appends the 2-byte tail and ciphers it. Legacy (paula_mode off):
//    [0..1]  opcode  (uint16_t)
//    [2..3]  length  (uint16_t) — total size including this 4-byte header
//    [4..]   payload
//
//  Usage (build outgoing):
//    PacketBuffer pkt(Opcode::SESSION_OPEN);
//    pkt.write<uint32_t>(42);
//    pkt.write_string("hello");
//    send(pkt.data(), pkt.size());
//
//  Usage (parse incoming):
//    PacketBuffer pkt(raw_bytes, raw_len);
//    auto op = pkt.opcode();
//    auto val = pkt.read<uint32_t>();
// ─────────────────────────────────────────────────────────────────────────────

class PacketBuffer
{
public:
    static constexpr size_t STANDARD_HEADER_SIZE = 4;    // opcode(2) + length(2)
    static constexpr size_t PAULA_HEADER_SIZE    = 9;    // len(2) + op(2) + seq(2) + key(1) + pack(1) + flag(1)
    static constexpr size_t PAULA_MIN_HEADER     = 9;    // Same as above
    static constexpr size_t PAULA_TAIL_SIZE      = 2;    // Season 2 CRC (2 bytes)
    static constexpr uint8_t SYNC_PACKET         = 0xDF;
    static constexpr size_t MAX_PACKET           = 65535; // max uint16_t

    // ── Construct for writing (outgoing packet) ───────────────────────────
    explicit PacketBuffer(Opcode op, bool is_paula = false)
        : is_paula_(is_paula), header_size_(is_paula ? PAULA_HEADER_SIZE : STANDARD_HEADER_SIZE)
    {
        buf_.resize(header_size_, 0);
        if (is_paula_) {
            // Trickster Opcode is at index 2 for Paula
            write_at<uint16_t>(2, static_cast<uint16_t>(op));
        } else {
            write_at<uint16_t>(0, static_cast<uint16_t>(op));
        }
        rpos_ = header_size_;
    }

    // ── Construct from raw bytes (incoming packet) ────────────────────────
    PacketBuffer(const uint8_t* data, size_t len, bool is_paula = false, size_t header_size = 0)
        : buf_(data, data + len), 
          is_paula_(is_paula), 
          header_size_(header_size != 0 ? header_size : (is_paula ? PAULA_HEADER_SIZE : STANDARD_HEADER_SIZE)),
          rpos_(header_size != 0 ? header_size : (is_paula ? PAULA_HEADER_SIZE : STANDARD_HEADER_SIZE))
    {
        if (len < header_size_)
            throw std::runtime_error("packet too small");
    }

    // ── Finalise: patch length field, returns span to send ───────────────
    void finalise()
    {
        if (is_paula_) {
            write_at<uint16_t>(0, static_cast<uint16_t>(buf_.size()));
        } else {
            write_at<uint16_t>(2, static_cast<uint16_t>(buf_.size()));
        }
    }

    // ── Command-block id (Paula header word @4) ───────────────────────────
    //  The client's frame extractor (sub_82A050) reads header word @4 as the "block id"
    //  (param). The event router (sub_6EB350): block id 0 => dispatch the packet NOW
    //  (ordinary packet); non-zero => COLLECT it into a command block keyed by that id and
    //  only run the block when its CLOSE (opcode 0x079F) arrives. The block runner
    //  (sub_6EBF20) plays an action from the block's FIRST packet (0x001F => melee attack,
    //  0x00E0/0x0124 => skill, 0x005B/0x0097 => item). So an attack only animates when its
    //  0x001F arrives first in a non-zero block that is then closed. Ordinary packets MUST
    //  carry 0 — we previously wrote an incrementing counter here, which made the client
    //  collect every packet into a one-packet block that never closed (the real cause of the
    //  "one-behind" we papered over with 0x34 fillers). Default 0 = correct.
    void        set_block_id(uint16_t b) { block_id_ = b; }
    uint16_t    block_id() const { return block_id_; }

    // ── Accessors ─────────────────────────────────────────────────────────
    Opcode      opcode() const { return static_cast<Opcode>(read_at<uint16_t>(is_paula_ ? 2 : 0)); }
    uint16_t    length() const { return read_at<uint16_t>(is_paula_ ? 0 : 2); }
    const uint8_t* data() const { return buf_.data(); }
    size_t      size()   const { return buf_.size(); }
    bool        empty()  const { return buf_.size() == header_size_; }
    size_t      rpos()   const { return rpos_; }
    size_t      header_size() const { return header_size_; }
    bool        at_end() const { return rpos_ >= buf_.size(); }
    bool        is_paula() const { return is_paula_; }

    // ── Write primitives (append to payload) ──────────────────────────────
    template<typename T>
    void write(T val)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const uint8_t*>(&val);
        buf_.insert(buf_.end(), p, p + sizeof(T));
    }

    void write_bytes(const void* src, size_t len)
    {
        const auto* p = static_cast<const uint8_t*>(src);
        buf_.insert(buf_.end(), p, p + len);
    }

    // Null-padded fixed-length string (like rAthena's STRING_SIZE fields)
    void write_string_fixed(const std::string& s, size_t fixed_len)
    {
        size_t copy = std::min(s.size(), fixed_len - 1);
        buf_.insert(buf_.end(), s.begin(), s.begin() + copy);
        buf_.insert(buf_.end(), fixed_len - copy, 0);
    }

    // Length-prefixed string (uint16_t length + bytes, no null terminator)
    void write_string(const std::string& s)
    {
        write<uint16_t>(static_cast<uint16_t>(s.size()));
        write_bytes(s.data(), s.size());
    }

    // ── Read primitives (sequential, advances rpos) ───────────────────────
    template<typename T>
    T read()
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (rpos_ + sizeof(T) > buf_.size())
            throw std::out_of_range("packet read overflow");
        T val;
        std::memcpy(&val, buf_.data() + rpos_, sizeof(T));
        rpos_ += sizeof(T);
        return val;
    }

    std::string read_string_fixed(size_t fixed_len)
    {
        if (rpos_ + fixed_len > buf_.size())
            throw std::out_of_range("packet read overflow");
        std::string s(reinterpret_cast<const char*>(buf_.data() + rpos_), fixed_len);
        rpos_ += fixed_len;
        s.resize(s.find('\0') == std::string::npos ? fixed_len : s.find('\0'));
        return s;
    }

    std::string read_string()
    {
        uint16_t len = read<uint16_t>();
        if (rpos_ + len > buf_.size())
            throw std::out_of_range("packet read overflow");
        std::string s(reinterpret_cast<const char*>(buf_.data() + rpos_), len);
        rpos_ += len;
        return s;
    }

    void skip(size_t n)
    {
        if (rpos_ + n > buf_.size())
            throw std::out_of_range("packet skip overflow");
        rpos_ += n;
    }

private:
    std::vector<uint8_t> buf_;
    bool is_paula_{ false };
    size_t header_size_{ STANDARD_HEADER_SIZE };
    size_t rpos_{ STANDARD_HEADER_SIZE };
    uint16_t block_id_{ 0 };   // Paula header word @4: 0 = ordinary packet; non-zero = command-block member

    template<typename T>
    void write_at(size_t offset, T val)
    {
        std::memcpy(buf_.data() + offset, &val, sizeof(T));
    }

    template<typename T>
    T read_at(size_t offset) const
    {
        T val;
        std::memcpy(&val, buf_.data() + offset, sizeof(T));
        return val;
    }
};
