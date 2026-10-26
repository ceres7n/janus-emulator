#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Sequential little-endian typed body codec — mirrors the cStream helpers used by
// the real servers (read u8/u16/i64/length-prefixed bytes). See spec 03.
namespace paula::inter {

class StreamWriter {
public:
    void u8(uint8_t v)   { buf_.push_back(v); }
    void u16(uint16_t v) { buf_.push_back(v & 0xFF); buf_.push_back((v >> 8) & 0xFF); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) buf_.push_back((v >> (8 * i)) & 0xFF); }
    void i64(int64_t v)  { uint64_t u = static_cast<uint64_t>(v);
                           for (int i = 0; i < 8; ++i) buf_.push_back((u >> (8 * i)) & 0xFF); }
    void bytes(const void* p, uint16_t n) {
        u16(n); const uint8_t* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    void str(const std::string& s) { bytes(s.data(), static_cast<uint16_t>(s.size())); }
    void raw(const uint8_t* p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); }
    void raw(const std::vector<uint8_t>& v)   { buf_.insert(buf_.end(), v.begin(), v.end()); }
    const std::vector<uint8_t>& data() const { return buf_; }
private:
    std::vector<uint8_t> buf_;
};

class StreamReader {
public:
    StreamReader(const uint8_t* p, std::size_t n) : p_(p), n_(n) {}
    explicit StreamReader(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}

    uint8_t u8() {
        if (pos_ + 1 > n_) { ok_ = false; return 0; }
        return p_[pos_++];
    }
    uint16_t u16() {
        if (pos_ + 2 > n_) { ok_ = false; return 0; }
        uint16_t v = static_cast<uint16_t>(p_[pos_] | (p_[pos_ + 1] << 8)); pos_ += 2; return v;
    }
    uint32_t u32() {
        if (pos_ + 4 > n_) { ok_ = false; return 0; }
        uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p_[pos_ + i]) << (8 * i);
        pos_ += 4; return v;
    }
    int64_t i64() {
        if (pos_ + 8 > n_) { ok_ = false; return 0; }
        uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p_[pos_ + i]) << (8 * i);
        pos_ += 8; return static_cast<int64_t>(v);
    }
    std::string str() {
        uint16_t n = u16();
        if (!ok_ || pos_ + n > n_) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + pos_), n); pos_ += n; return s;
    }
    bool ok() const { return ok_; }
    std::size_t remaining() const { return n_ > pos_ ? n_ - pos_ : 0; }
private:
    const uint8_t* p_; std::size_t n_; std::size_t pos_ = 0; bool ok_ = true;
};

} // namespace paula::inter
