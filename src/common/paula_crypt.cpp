#include "paula_crypt.hpp"
#include "logger.hpp"
#include <fstream>
#include <iostream>
#include <cstring>

namespace janus {

bool PaulaCrypt::LoadSbox(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        LOG_ERROR("[PaulaCrypt] Could not open S-box file: %s", path.c_str());
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size != 65536 && size != 262144) {
        LOG_ERROR("[PaulaCrypt] Invalid S-box size: %lld (expected 65536 or 262144)", static_cast<long long>(size));
        return false;
    }

    sbox_.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(sbox_.data()), size)) {
        LOG_ERROR("[PaulaCrypt] Failed to read S-box data from %s", path.c_str());
        sbox_.clear();                          // a half-read table must not count as loaded
        return false;
    }

    LOG_INFO("[PaulaCrypt] S-box loaded (%lld bytes)", static_cast<long long>(size));
    return true;
}

uint8_t PaulaCrypt::MakeChecksum(const uint8_t* packet, uint8_t key) const {
    if (sbox_.empty()) return 0;
    uint8_t x = sbox_[static_cast<uint16_t>((packet[0] << 8) + packet[2])];
    uint8_t bl = sbox_[static_cast<uint16_t>((key << 8) + x)];
    uint8_t y = sbox_[static_cast<uint16_t>((packet[3] << 8) + packet[1])];
    uint8_t al = sbox_[static_cast<uint16_t>((packet[6] << 8) + y)];
    return sbox_[static_cast<uint16_t>((bl << 8) + al)];
}

bool PaulaCrypt::ExcludeDummy(uint8_t* packet, uint16_t& out_len) {
    uint8_t packing = packet[7];
    if ((packing & 8) > 0) {
        uint16_t len; std::memcpy(&len, packet, 2);
        // In int: a u16 subtraction would wrap (11 - 12 = 65535) and pass the minimum check.
        const int n = static_cast<int>(len) - packet[6] % 13;
        if (n < 11) return false;
        const uint16_t lenNoDummy = static_cast<uint16_t>(n);
        std::memcpy(packet, &lenNoDummy, 2);
        packet[7] = packing ^ 8;
        out_len = lenNoDummy;
    }
    return true;
}

bool PaulaCrypt::UnpackHeader(uint8_t* packet, uint8_t& key, bool& is_first_packet) {
    if (sbox_.empty()) return false;

    // Save original encrypted bytes for brute-force diagnostics later.
    uint8_t orig[9];
    std::memcpy(orig, packet, 9);

    // Remember whether this is the first packet before the magic check clears the flag.
    // The first packet's opcode is always 0x2CED (KTO login request) — used to break
    // ties when the brute-force finds multiple checksum-valid candidates.
    bool was_first_packet = is_first_packet;

    // First Packet Validation magic
    if (is_first_packet) {
        uint8_t packing = packet[7];
        if (packing > 0x0F && (sbox_[static_cast<uint16_t>((packet[6] << 8) + packing)] & 0x0F) == 15) {
            is_first_packet = false;
        } else {
            return false;
        }
    }

    uint8_t packing = packet[7];

    if (packing > 0x0F) {
        uint8_t packetStatus = sbox_[static_cast<uint16_t>((packet[6] << 8) + packing)];
        // Note: we do NOT reject on packetStatus > 0x0F or missing bit-1 here.
        // TrickEmuS2 operates with IsStrict=false (the default), which skips this
        // validation gate entirely and relies solely on the checksum below.
        // Rejecting non-standard packing values breaks valid CoraTO client packets.

        uint16_t sequence = (sbox_[static_cast<uint16_t>(((packet[3] ^ packet[7]) << 8) + packet[5])] << 8) +
                            sbox_[static_cast<uint16_t>(((packet[2] ^ key) << 8) + packet[4])];

        uint16_t opcode = (sbox_[static_cast<uint16_t>(((packet[1] ^ key) << 8) + packet[3])] << 8) +
                          sbox_[static_cast<uint16_t>(((packet[6] ^ packet[0]) << 8) + packet[2])];

        uint16_t length = (sbox_[static_cast<uint16_t>(((packet[6] ^ packet[7]) << 8) + packet[1])] << 8) +
                          sbox_[static_cast<uint16_t>(((packet[7] ^ key) << 8) + packet[0])];

        packet[7] = packetStatus ^ 2;
        std::memcpy(packet + 0, &length, 2);
        std::memcpy(packet + 2, &opcode, 2);
        std::memcpy(packet + 4, &sequence, 2);
    }

    uint16_t finalLen; std::memcpy(&finalLen, packet, 2);
    uint32_t plain_size; std::memcpy(&plain_size, packet, 4);

    if (finalLen >= 0x0B && plain_size < 0x7FFFFFFF && MakeChecksum(packet, key) == packet[8]) {
        return true;
    }

    // Self-healing: brute-force all 256 keys using the saved original bytes.
    // Collect every candidate, then disambiguate:
    //   1. If exactly one candidate — use it unconditionally.
    //   2. If multiple — and this was the first packet — prefer the one whose
    //      decoded opcode == 0x2CED (KTO login request, the only valid first packet).
    //   3. If still ambiguous — log and reject.
    {
        struct Candidate { int key; uint8_t decoded[9]; };
        Candidate candidates[256];
        int found_count = 0;

        for (int k = 0; k < 256; ++k) {
            uint8_t tmp[9];
            std::memcpy(tmp, orig, 9);

            uint8_t p = tmp[7];
            if (p > 0x0F) {
                uint8_t ps = sbox_[static_cast<uint16_t>((tmp[6] << 8) + p)];
                uint16_t seq = (sbox_[static_cast<uint16_t>(((tmp[3] ^ tmp[7]) << 8) + tmp[5])] << 8) +
                               sbox_[static_cast<uint16_t>(((tmp[2] ^ k)      << 8) + tmp[4])];
                uint16_t op  = (sbox_[static_cast<uint16_t>(((tmp[1] ^ k)      << 8) + tmp[3])] << 8) +
                               sbox_[static_cast<uint16_t>(((tmp[6] ^ tmp[0]) << 8) + tmp[2])];
                uint16_t len = (sbox_[static_cast<uint16_t>(((tmp[6] ^ tmp[7]) << 8) + tmp[1])] << 8) +
                               sbox_[static_cast<uint16_t>(((tmp[7] ^ k)      << 8) + tmp[0])];
                tmp[7] = ps ^ 2;
                std::memcpy(tmp + 0, &len, 2);
                std::memcpy(tmp + 2, &op,  2);
                std::memcpy(tmp + 4, &seq, 2);
            }

            uint16_t fl; std::memcpy(&fl, tmp, 2);
            uint32_t sz; std::memcpy(&sz, tmp, 4);
            if (fl >= 0x0B && sz < 0x7FFFFFFF && MakeChecksum(tmp, static_cast<uint8_t>(k)) == tmp[8]) {
                candidates[found_count].key = k;
                std::memcpy(candidates[found_count].decoded, tmp, 9);
                ++found_count;
            }
        }

        // (DEBUG: every connection's first packet heals from the initial key, so these are routine)
        auto accept = [&](int idx) {
            uint16_t healed_op; std::memcpy(&healed_op, candidates[idx].decoded + 2, 2);
            LOG_DEBUG("UnpackHeader: key healed 0x%02X→0x%02X (opcode=0x%04X)",
                      key, static_cast<uint8_t>(candidates[idx].key), healed_op);
            key = static_cast<uint8_t>(candidates[idx].key);
            std::memcpy(packet, candidates[idx].decoded, 9);
        };

        if (found_count == 1) {
            accept(0);
            return true;
        }

        if (found_count > 1 && was_first_packet) {
            // Login first packet: opcode 0x2CED uniquely identifies it.
            for (int i = 0; i < found_count; ++i) {
                uint16_t op; std::memcpy(&op, candidates[i].decoded + 2, 2);
                if (op == 0x2CED) {
                    LOG_DEBUG("UnpackHeader: ambiguous (%d keys), resolved by opcode 0x2CED", found_count);
                    accept(i);
                    return true;
                }
            }
            // Non-login first packet (e.g. char server): opcode unknown.
            // Fall back to K_init=0x01 — observed constant for this client build.
            for (int i = 0; i < found_count; ++i) {
                if (candidates[i].key == 0x01) {
                    LOG_DEBUG("UnpackHeader: ambiguous (%d keys), resolved by K_init=0x01 fallback", found_count);
                    accept(i);
                    return true;
                }
            }
        }

        if (found_count == 0)
            LOG_WARN("UnpackHeader: checksum FAIL key=0x%02X — no valid key (malformed?)", key);
        else
            LOG_WARN("UnpackHeader: checksum FAIL key=0x%02X — %d keys match, ambiguous", key, found_count);
    }

    return false;
}

bool PaulaCrypt::UnpackData(uint8_t* packet, size_t buf_len, uint16_t total_len, uint8_t& current_key) {
    if (sbox_.empty()) return false;
    // header (9) + tail (2) at least, and never past the bytes the caller holds
    if (total_len < 11 || total_len > buf_len) return false;

    uint16_t cmd; std::memcpy(&cmd, packet + 2, 2);
    uint8_t randKey = packet[6];
    uint8_t packing = packet[7];
    uint8_t magicKey = static_cast<uint8_t>((cmd * randKey) % 256);
    
    uint16_t header_len = total_len - 2; 
    int offset = 0;
    uint16_t calculatedTail = 0;

    if ((packing & 4) > 0) {
        if (header_len != 9) {
            do {
                uint8_t decrypted = sbox_[static_cast<uint16_t>(((magicKey ^ offset) << 8) + packet[offset + 9])];
                packet[offset + 9] = decrypted;
                calculatedTail += decrypted;
                offset++;
            } while (offset < header_len - 9);
        }
        
        for (int i = 0; i < 2; i++) {
            uint8_t decrypted = sbox_[static_cast<uint16_t>(((magicKey ^ offset) << 8) + packet[offset + 9])];
            packet[offset + 9] = decrypted;
            offset++;
        }
        packet[7] ^= 4; 
    } else {
        offset = header_len - 7;
        calculatedTail = static_cast<uint16_t>(header_len - 9);
    }

    uint16_t realTail; std::memcpy(&realTail, packet + total_len - 2, 2);
    if (calculatedTail != realTail) return false;

    // Advance the C2S key after successful body decryption.
    // Mirrors client sub_AB16B0: key += sbox[(plaintext_sum_lo << 8) + key]
    // Condition: packing bit 0 = key-advance flag (always set for KTO clients: flags=0x0F → decoded packing=0x0D).
    if ((packing & 1) != 0) {
        current_key = static_cast<uint8_t>(
            sbox_[static_cast<uint16_t>(((calculatedTail & 0xFF) << 8) + current_key)] + current_key);
    }
    return true;
}

void PaulaCrypt::EncryptHeader(uint8_t* packet, uint8_t key) const {
    if (sbox_.empty()) return;
    uint8_t randKey = static_cast<uint8_t>(std::rand() % 256);
    packet[6] = randKey;
    packet[7] = 0x06; 
    packet[8] = MakeChecksum(packet, key);

    packet[7] = sbox_[static_cast<uint16_t>((randKey << 8) + packet[7])];

    uint16_t length; std::memcpy(&length, packet, 2);
    uint16_t encLength = (sbox_[static_cast<uint16_t>(((packet[6] ^ packet[7]) << 8) + packet[1])] << 8) +
                          sbox_[static_cast<uint16_t>(((packet[7] ^ key) << 8) + packet[0])];
    std::memcpy(packet + 0, &encLength, 2);

    uint16_t opcode; std::memcpy(&opcode, packet + 2, 2);
    uint16_t encOpcode = (sbox_[static_cast<uint16_t>(((packet[1] ^ key) << 8) + packet[3])] << 8) +
                          sbox_[static_cast<uint16_t>(((packet[6] ^ packet[0]) << 8) + packet[2])];
    std::memcpy(packet + 2, &encOpcode, 2);

    uint16_t sequence; std::memcpy(&sequence, packet + 4, 2);
    uint16_t encSequence = (sbox_[static_cast<uint16_t>(((packet[3] ^ packet[7]) << 8) + packet[5])] << 8) +
                            sbox_[static_cast<uint16_t>(((packet[2] ^ key) << 8) + packet[4])];
    std::memcpy(packet + 4, &encSequence, 2);
}

void PaulaCrypt::EncryptData(uint8_t* packet, uint16_t length, uint16_t opcode, uint8_t& current_key) const {
    if (sbox_.empty()) return;
    
    uint8_t magicKey = static_cast<uint8_t>((opcode * packet[6]) % 256);
    uint16_t tail = 0;
    int offset = 0;
    
    for (; offset < length - 11; offset++) {
        tail += packet[offset + 9];
        packet[offset + 9] = sbox_[static_cast<uint16_t>(((magicKey ^ offset) << 8) + packet[offset + 9])];
    }
    
    // S2C key (paula_send_key_) is static — client network_object[9] never advances.
    // Do NOT advance current_key here; C2S key is independent and advanced in UnpackData.

    int seq = offset;
    packet[length - 2] = static_cast<uint8_t>(tail & 0xFF);
    packet[length - 1] = static_cast<uint8_t>((tail >> 8) & 0xFF);
    
    for (int i = 0; i < 2; i++, seq++) {
        packet[i + offset + 9] = sbox_[static_cast<uint16_t>(((magicKey ^ seq) << 8) + packet[length - 2 + i])];
    }
}

} // namespace janus
