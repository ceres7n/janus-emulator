#pragma once
#include <cstdint>
#include "packet.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  How an item travels on the wire (retail GameServer / KR client, verified on both sides).
// ─────────────────────────────────────────────────────────────────────────────

// An item's data (retail sub_60C0C0; client sub_6B39A0): kind 1 (common) -> [u32 id][u64 uid]
// [u16 count] (sub_60D930 / client sub_6B34B0, 14 bytes); kind 2 or 3 (rare) -> the same plus the
// rare item's 4 bytes (+48..+51; sub_60D970 / client sub_6B3550, 18 bytes).
inline void write_item_data(PacketBuffer& p, uint8_t kind, uint32_t id, uint64_t uid, uint16_t count,
                            const uint8_t* rare4 = nullptr)
{
    p.write<uint32_t>(id);
    p.write<uint64_t>(uid);
    p.write<uint16_t>(count);
    if (kind == 2 || kind == 3)
        for (int i = 0; i < 4; ++i) p.write<uint8_t>(rare4 ? rare4[i] : 0);
}
// (A rare item's options — the extra info after a kind-2 item with a uid — item_option.hpp.)
