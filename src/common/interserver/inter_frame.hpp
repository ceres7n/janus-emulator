#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

// Inter-server frame codec — PLAINTEXT (flags=0), mirroring the recovered Paula
// peer link (reverse/specs/03_interserver_transport.md).
//
// Wire: [len u16][opcode u16][field2 u16][flags u8][key u8][hdrChk u8] body [bodyChk u16]
//   total len = body + 11  (header 9 + body-checksum 2);  flags=0 => no cipher, no pad.
//
// The real binaries' exact checksum algorithms are [?] and only matter if we need to
// wire-interop with them. A pure-ProjectPAULA cluster controls both ends, so we use a
// consistent running-sum checksum here.
namespace paula::inter {

constexpr std::size_t kHeaderSize = 9;
constexpr std::size_t kOverhead   = 11;   // header(9) + bodyChecksum(2)

struct Frame {
    uint16_t opcode = 0;
    uint16_t field2 = 0;
    std::vector<uint8_t> body;
};

inline uint8_t header_checksum(const uint8_t* h) {                 // over bytes [0..7]
    unsigned s = 0; for (int i = 0; i < 8; ++i) s += h[i];
    return static_cast<uint8_t>(s & 0xFF);
}
inline uint16_t body_checksum(const uint8_t* b, std::size_t n) {
    unsigned s = 0; for (std::size_t i = 0; i < n; ++i) s += b[i];
    return static_cast<uint16_t>(s & 0xFFFF);
}

inline std::vector<uint8_t> encode(uint16_t opcode, const uint8_t* body,
                                   std::size_t n, uint16_t field2 = 0) {
    const std::size_t total = kOverhead + n;
    std::vector<uint8_t> f(total, 0);
    f[0] = static_cast<uint8_t>(total & 0xFF);
    f[1] = static_cast<uint8_t>((total >> 8) & 0xFF);
    f[2] = static_cast<uint8_t>(opcode & 0xFF);
    f[3] = static_cast<uint8_t>((opcode >> 8) & 0xFF);
    f[4] = static_cast<uint8_t>(field2 & 0xFF);
    f[5] = static_cast<uint8_t>((field2 >> 8) & 0xFF);
    f[6] = 0;  // flags = 0 (plaintext)
    f[7] = 0;  // key
    f[8] = header_checksum(f.data());
    if (n) std::memcpy(f.data() + kHeaderSize, body, n);
    uint16_t bc = body_checksum(body, n);
    f[kHeaderSize + n]     = static_cast<uint8_t>(bc & 0xFF);
    f[kHeaderSize + n + 1] = static_cast<uint8_t>((bc >> 8) & 0xFF);
    return f;
}
inline std::vector<uint8_t> encode(uint16_t opcode, const std::vector<uint8_t>& body,
                                   uint16_t field2 = 0) {
    return encode(opcode, body.data(), body.size(), field2);
}

// Try to extract one frame from the front of `buf`.
//   returns >0 = consumed bytes (out filled) · 0 = need more · -1 = corrupt (drop link)
inline int try_decode(const uint8_t* buf, std::size_t avail, Frame& out) {
    if (avail < kHeaderSize) return 0;
    uint16_t len = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
    if (len < kOverhead) return -1;
    if (avail < len) return 0;
    if (header_checksum(buf) != buf[8]) return -1;
    std::size_t bodyLen = len - kOverhead;
    const uint8_t* b = buf + kHeaderSize;
    uint16_t bc = static_cast<uint16_t>(buf[len - 2] | (buf[len - 1] << 8));
    if (body_checksum(b, bodyLen) != bc) return -1;
    out.opcode = static_cast<uint16_t>(buf[2] | (buf[3] << 8));
    out.field2 = static_cast<uint16_t>(buf[4] | (buf[5] << 8));
    out.body.assign(b, b + bodyLen);
    return static_cast<int>(len);
}

} // namespace paula::inter
