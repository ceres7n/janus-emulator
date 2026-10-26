#ifndef PAULA_CRYPT_HPP
#define PAULA_CRYPT_HPP

#include <vector>
#include <string>
#include <cstdint>

namespace janus {

class PaulaCrypt {
public:
    static PaulaCrypt& Instance() {
        static PaulaCrypt instance;
        return instance;
    }

    bool LoadSbox(const std::string& path);
    // Unpacks the header (first 9 bytes) and sets out_len, out_op, out_seq. Alters packet in-place.
    // key is in/out: updated to the correct key if self-healing brute-force succeeds.
    bool UnpackHeader(uint8_t* packet, uint8_t& key, bool& is_first_packet);
    // Excludes dummy sizing. Alters out_len if dummy bytes existed.
    bool ExcludeDummy(uint8_t* packet, uint16_t& out_len);
    // Unpacks the body and verifies the 2-byte tail. buf_len = the bytes packet holds; false when the
    // logical total_len does not fit in them.
    bool UnpackData(uint8_t* packet, size_t buf_len, uint16_t total_len, uint8_t& current_key);
    
    // Outbound Packing
    void EncryptHeader(uint8_t* packet, uint8_t key) const;
    void EncryptData(uint8_t* packet, uint16_t length, uint16_t opcode, uint8_t& current_key) const;

    // Checksum calculator
    uint8_t MakeChecksum(const uint8_t* packet, uint8_t key) const;

    bool IsLoaded() const { return !sbox_.empty(); }

private:
    PaulaCrypt() = default;
    std::vector<uint8_t> sbox_; // 65536-byte (256x256) substitution table
};

} // namespace janus

#endif // PAULA_CRYPT_HPP
