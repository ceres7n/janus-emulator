#pragma once
#include <cstdint>
#include <vector>
#include "packet.hpp"
#include "world/item_db.hpp"
#include "world/random_stat_db.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  A rare item's options (ItemOption, world/item_db.hpp): how a new rare item gets them and how
//  they travel on the wire.
// ─────────────────────────────────────────────────────────────────────────────

// sub_6EC4E0: the stat index of a type-10 option id (10000..10011 = ItemParam APPlus..HVPlus), else -1.
inline int item_plus_index(int id) { return (id >= 10000 && id <= 10011) ? id - 10000 : -1; }

// sub_6E0090: an item's base options — its non-zero APPlus..HVPlus (10000+i), a gun's AP (SubType 54,
// param +12; 10012 = GunAp), its HP/MP recovery rates (24000 / 24001). (A per-item extra list,
// GameInfoMgr unk_F03268, is read here too; nothing ever fills it.)
inline void base_item_options(const ItemDef& d, std::vector<ItemOption>& out)
{
    for (int i = 0; i < IP_COUNT; ++i)
        if (d.plus[i]) out.push_back({ 10, static_cast<int16_t>(10000 + i), d.plus[i], 0 });
    if (d.sub_type == 54) out.push_back({ 10, 10012, d.ap, 0 });
    if (d.hp_recovery_rate != 0.0f)
        out.push_back({ 10, 24000, static_cast<int16_t>(static_cast<int>(d.hp_recovery_rate)), 0 });
    if (d.mp_recovery_rate != 0.0f)
        out.push_back({ 10, 24001, static_cast<int16_t>(static_cast<int>(d.mp_recovery_rate)), 0 });
}

// sub_6E02D0 (called by MakeItemDefaultStatHelper sub_6E0490 for every new rare item, random =
// OptionsEx & 1): the base options, then — when random and the item rolls stats (OptionsEx & 1,
// sub_617860) — each rolled stat replaces the base option of the same id or is appended.
inline std::vector<ItemOption> make_item_options(const ItemDef& d, const RandomStatDb& rs, bool random)
{
    std::vector<ItemOption> out;
    base_item_options(d, out);
    const size_t base = out.size();
    if (random) {
        std::vector<RandomStatRoll> rolled;
        if (d.options_ex & 1) rs.roll_stats(d.id, rolled);
        for (const RandomStatRoll& r : rolled) {
            size_t j = 0;
            for (; j < base; ++j)
                if (out[j].id == r.id) { out[j].value = r.value; break; }
            if (j >= base) out.push_back({ 10, r.id, r.value, 0 });
        }
    }
    return out;
}

// The extra info of a rare (kind 2) item with a uid (retail sub_6EC150; client sub_593690 ->
// sub_593570): [u8 n] + n x {u32 type, u32 id | value << 16, u32 extra} — every option except a
// type-10 stat 10000..10011 whose value is the item's own base value (the client has those).
inline void write_rare_extra(PacketBuffer& p, const ItemDef* d, const std::vector<ItemOption>& opts)
{
    uint8_t n = 0;
    for (const ItemOption& o : opts) {
        const int k = item_plus_index(o.id);
        if (o.type == 10 && k >= 0 && d && d->plus[k] == o.value) continue;
        ++n;
    }
    p.write<uint8_t>(n);
    for (const ItemOption& o : opts) {
        const int k = item_plus_index(o.id);
        if (o.type == 10 && k >= 0 && d && d->plus[k] == o.value) continue;
        p.write<uint32_t>(o.type);
        p.write<uint32_t>(static_cast<uint32_t>(static_cast<uint16_t>(o.id)) |
                          (static_cast<uint32_t>(static_cast<uint16_t>(o.value)) << 16));
        p.write<uint32_t>(static_cast<uint32_t>(o.extra));
    }
}
