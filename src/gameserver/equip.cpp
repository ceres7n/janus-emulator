#include "game_session.hpp"
#include "game_server.hpp"
#include "combat_util.hpp"
#include "logger.hpp"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  Equipment — retail GameServer, verified against the KR client.
//
//  The 23 equipment slots (Player +2120) reference items that stay in the bag (+1716): at login the
//  DB's equipped items (item_where 0, item_pos = slot) and the bag's (item_where 1) are one list
//  (S2C 0x00E7) and S2C 0x00E9 starts with the 23 equipped uids. The slot of an item is its Type's
//  (ConvertToEquipType sub_6DA3A0, table dword_AB5778 — the client's dword_F602F8 is the same); an
//  accessory (Type 7) goes to 4, else 9, a SubType 53 one to 22 first (sub_6DA520).
//
//  C2S 0x0051 CMD_EQUIP_ITEM [u64 uid][u8 slot] (Player case 81): the item from the bag; Player::
//    EquipItem (vtbl+292 sub_763340): VerifyEquipItem (sub_762A00), then the item already in that
//    slot is released with it (PrepareReleaseBeforeEquipRareItem sub_762290), hItemEquipment (DB:
//    uspm_item_char_update_move_inven_to_equip / ..._exchange_inven_to_equip) -> OnOk (sub_782370)
//    -> EquipNReleaseItem (vtbl+344 sub_763FD0). Any failure: S2C 0x0053 [u64 uid].
//  C2S 0x0054 CMD_RELEASE_ITEM [u64 uid][u8 slot] (case 84): an equipped item; VerifyReleaseItem
//    (sub_763170: ears/tail — Type 17/18 — stay on below level 20); DB; EquipNReleaseItem with the
//    item to release only. Failure: S2C 0x0056 [u64 uid].
//  EquipNReleaseItem: each released item leaves its slot (its stats go, sub_6DF9D0) — S2C 0x0055
//    [u64 uid][u8 slot] to us, S2C 0x00ED [u16 user][u32 item id] to the area (sub_5F6060) — then the
//    new one takes the slot (sub_763B10: its stats come, sub_6DF930) — S2C 0x0052 [u64 uid][u8 slot]
//    to us, S2C 0x00EC [u16 user][u32 item id][u32 shown item id] to the area (sub_5F5F80). The
//    client equips/re-computes its own stats (sub_6AF6F0 / sub_6AE830) and shows the others' change
//    (sub_6F0130); our own 0x00EC/0x00ED are ignored by the client.
//  Item stats (sub_718750): for each of the 12 stats, the item's own options of that id (10000+i,
//    summed over its option types, sub_6E8140) when it has any, else ItemParam APPlus..HVPlus; added
//    to Player +608 and so to the final stats (sub_716680).
//  The common slot 20 (C2S 0x0205 / 0x0208, S2C 0x0206 / 0x0207 / 0x0209 / 0x020A): bullets (Type 29)
//    into the magazine, a Throw item (Type 16) or throw card (Type 11 SubType 60) into the throw
//    slot — neither is one of the 23 slots; a gun's equip effect: gun.cpp.
//  The 22 element values (Character +836 + 2i, sub_718280): an item's options of the slot's id (sub_6EC3D0:
//    Fire..NoProp 20000..20007, Shadow 20008, FireR..NoPropR 21000..21007, GunR 21009, PhysicalR 21008,
//    ShadowR 21010) go to +880 on equip (sub_718900) — elem_slot. The ItemParam-less source of such options,
//    the Item_Attribute table (sub_656BD0: option type 10 records per ID), is not attached to our items yet.
//  Not here yet: set items (sub_764D70), SpeedItemParam / the SubType 62 table (sub_717E70), a rare
//    item's look option (type 12, the second appearance mask).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// dword_AB5778: equip slot of an item Type (ConvertToEquipType sub_6DA3A0), -1 = not equippable.
constexpr int kEquipTypeSlot[36] = { -1, -1, 1, 0, 3, 18, 7, 4, 2, -1, -1, -1, -1, 8, -1, -1, -1, 5, 6, 10, 11, 12,
                                     13, 14, 15, 16, 17, 19, -1, -1, -1, -1, -1, -1, 21, -1 };

int convert_to_equip_type(int type)
{
    return (type > 0 && type < 36) ? kEquipTypeSlot[type] : -1;
}

} // namespace

bool GameClientSession::is_equipped(uint64_t uid) const
{
    if (!uid) return false;
    for (uint64_t e : equip_) if (e == uid) return true;
    return false;
}

// sub_6DA520: the slot for an item; an accessory takes 22 (SubType 53, when free), 4, 9, else 4.
int GameClientSession::equip_slot_for(const ItemDef& d) const
{
    const int slot = convert_to_equip_type(d.type);
    if (d.type != 7) return slot;
    if (d.sub_type == 53 && !equip_[22]) return 22;
    if (!equip_[slot]) return slot;
    if (!equip_[9]) return 9;
    return slot;
}

// sub_718750 over every equipped item: the 12 stat bonuses (Player +608).
void GameClientSession::equip_bonus(int32_t out[12]) const
{
    for (int i = 0; i < 12; ++i) out[i] = 0;
    for (uint64_t uid : equip_) {
        if (!uid) continue;
        const BagItem* b = nullptr;
        for (const BagItem& it : bag_) if (it.uid == uid) { b = &it; break; }
        if (!b || !b->item_id) continue;
        const ItemDef* d = server_.item_db().get(b->item_id);
        if (!d) continue;
        for (int i = 0; i < 12; ++i) {
            int32_t v = d->plus[i];
            bool have = false;
            int32_t sum = 0;
            for (const ItemOption& o : b->opts)                          // sub_6E8140(uid, 10000 + i)
                if (o.id == 10000 + i) { sum += o.value; have = true; }
            if (have) v = static_cast<int16_t>(sum);
            out[i] += v;
        }
    }
}

// Character +836 + 2i (sub_718280) of a player: +1448 (never written: 0) + +924 + +880 + +968 — +880 the
// equipped items' options of the slot's id (sub_718900 on each equip: sub_6EC3D0(i), the item's options of
// it summed by sub_6E8140 when it has any), +924 the ES adders (elem_es_: the Bard's shields; ESAElementUpDo
// not done yet), +968 those of the ESA*AreaDo classes (not done yet); the resistances (11..21) at most 70.
// (Slots 8 / 9 have no option.)
int16_t GameClientSession::elem_slot(int i) const
{
    static constexpr int kOpt[22] = { 20000, 20001, 20002, 20003, 20004, 20005, 20006, 20007, 0, 0, 20008,
                                      21000, 21001, 21002, 21003, 21004, 21005, 21006, 21007, 21009, 21008, 21010 };
    if (i < 0 || i >= 22) return 0;
    int16_t v = 0;
    if (kOpt[i]) {
        for (uint64_t uid : equip_) {
            if (!uid) continue;
            const BagItem* b = nullptr;
            for (const BagItem& it : bag_) if (it.uid == uid) { b = &it; break; }
            if (!b) continue;
            bool have = false;
            int16_t sum = 0;
            for (const ItemOption& o : b->opts)                          // sub_6E8140(uid, sub_6EC3D0(i))
                if (o.id == kOpt[i]) { sum = static_cast<int16_t>(sum + o.value); have = true; }
            if (have) v = static_cast<int16_t>(v + sum);                 // sub_7188C0: +880 += it
        }
    }
    v = static_cast<int16_t>(v + elem_es_[i]);
    if (i >= 11 && v > 70) v = 70;
    return v;
}

// Player::VerifyEquipItem (rare items, sub_762A00).
bool GameClientSession::verify_equip(const BagItem& b, const ItemDef& d, int slot) const
{
    auto deny = [&](const char* why) {
        LOG_INFO("[ITEMEX] Player::VerifyEquipItem(Rare) : %s: %s, est:%d, itmUid:%llu, itemIndex:%u", why,
                 char_name_, slot, static_cast<unsigned long long>(b.uid), d.id);
        return false;
    };
    // a GM-only item (OptionsEx 1024) only for a GM (sub_5C1650: Player +1640 flag 1)
    if (!authority_ && (d.options_ex & 1024)) return deny("GM item");
    // (the ChrStateFlag checks, Player +1060 flags 1/16 -> S2C 0x01E4 7/17: no such state here)
    // Equipable (sub_6DE140): Options 16, not broken (rare byte 2 == 1) unless a CM item (0x40000)
    const bool broken = (b.kind == 2) && b.rare[2] == 1;
    if (!(d.options & 16) || (broken && !(d.options & 0x40000))) return deny("Not Equipable");
    auto item_at = [&](int s) -> const ItemDef* {
        if (s < 0 || s >= 23 || !equip_[s]) return nullptr;
        for (const BagItem& it : bag_) if (it.uid == equip_[s]) return server_.item_db().get(it.item_id);
        return nullptr;
    };
    const bool acc_slot = (slot == 4 || slot == 9 || slot == 22);
    if (d.options & 0x4000) {
        if (acc_slot && d.chr_type_flags)                                    // sub_6DA660
            for (int s : { 4, 9, 22 })
                if (const ItemDef* o = item_at(s)) if (o->chr_type_flags == d.chr_type_flags) return false;
        if (!(type_ >= 1 && type_ < 25 && (d.chr_type_flags & (1u << (type_ - 1)))))   // sub_6DE080
            return deny("Invalid eChrType");
    }
    if (acc_slot && (d.sub_type == 52 || d.sub_type == 53 || d.sub_type == 62))   // sub_6DA950
        for (int s : { 4, 9, 1, 22 })
            if (const ItemDef* o = item_at(s)) if (o->sub_type == d.sub_type) return false;
    if (slot == -1) return false;
    if (is_equipped(b.uid)) return deny("Already Equiped");
    if (level_ < d.min_level) return deny("Invalid Level");
    if (d.min_stat_lv && d.min_stat_type >= 0 && d.min_stat_type < 12 && stat_[d.min_stat_type] < d.min_stat_lv)
        return deny("Invalid Stat");
    for (int s : { 4, 9, 22 })                                               // sub_6DA5F0
        if (const ItemDef* o = item_at(s)) if (o->id == d.id) return deny("m_EquipSlot.IsExist failed");
    if (d.type == 7) {
        if (!acc_slot) return deny("eItemType_Accessory1 slot");
    } else if (slot != equip_slot_for(d)) {
        return deny("not the item's slot");
    }
    // a gun (SubType 54) needs the PSAbility 1 in m_ESLimiter (Player +4504, sub_5255F0: the passive
    // Gun Carrier, skill 3008); a shield (Type 8) never beside a gun (sub_6DA700)
    if (d.sub_type == 54 && !has_ps_ability(1)) return false;
    if (d.type == 8 && gun_in_any_slot()) return false;
    return true;
}

// Player::VerifyEquipItem(Common) (sub_762720): a GM-only item only for a GM; (the ChrStateFlag
// checks); Equipable; the character type of an Options 0x4000 item; a slot; MinLevel; MinStatType /
// MinStatLv; then bullets (Type 29) need a gun in 4/9/1/22 and not the same bullets loaded already,
// a Throw item (16) or a card (11) not the same item in the throw slot. (The slot itself is not
// checked: the client always asks for 20.)
bool GameClientSession::verify_equip_common(const BagItem& b, const ItemDef& d, int slot) const
{
    if (!authority_ && (d.options_ex & 1024)) return false;
    const bool broken = (b.kind == 2) && b.rare[2] == 1;                     // sub_6DE140
    if (!(d.options & 16) || (broken && !(d.options & 0x40000))) return false;
    if ((d.options & 0x4000) && !(type_ >= 1 && type_ < 25 && (d.chr_type_flags & (1u << (type_ - 1)))))
        return false;
    if (slot == -1) return false;
    if (level_ < d.min_level) return false;
    if (d.min_stat_lv && d.min_stat_type >= 0 && d.min_stat_type < 12 && stat_[d.min_stat_type] < d.min_stat_lv)
        return false;
    if (d.type == 29) {
        if (!gun_in_any_slot()) return false;
        if (mag_.item) {
            for (const BagItem& it : bag_)
                if (it.uid == mag_.item && it.item_id == d.id) return false;
        }
        return true;
    }
    if (d.type == 16 || d.type == 11) {
        if (throw_item_ && throw_item_ == b.uid) {
            LOG_INFO("[ITEMEX] Player::VerifyEquipItem(Common) : %s Already Equiped: %s, est:%d, itmUid:%llu, itemIndex:%u",
                     d.type == 16 ? "eItemType_Throw" : "eItemType_Card", char_name_, slot,
                     static_cast<unsigned long long>(b.uid), d.id);
            return false;
        }
        return true;
    }
    LOG_INFO("[ITEMEX] Player::VerifyEquipItem(Common) : invalid item : est:%d, itmUid:%llu, itemIndex:%u", slot,
             static_cast<unsigned long long>(b.uid), d.id);
    return false;
}

// Player::VerifyReleaseItem for a common item (sub_762EF0): (the ChrStateFlag checks); slot 20 and
// the item is what the slot holds — the loaded stack for bullets, the throw item for a Throw item or
// a throw card (Type 11 SubType 60).
bool GameClientSession::verify_release_common(const BagItem& b, const ItemDef& d, int slot) const
{
    if (slot == -1) return false;
    const char* what = nullptr;
    if (d.type == 29) {
        if (slot != 20 || !mag_.item) return false;
        if (mag_.item == b.uid) return true;
        what = "BULLET";
    } else if (d.type == 16) {
        if (slot != 20) return false;
        if (throw_item_ && throw_item_ == b.uid) return true;
        what = "eItemType_Throw";
    } else if (d.type == 11) {
        if (slot != 20 || d.sub_type != 60) return false;
        if (throw_item_ && throw_item_ == b.uid) return true;
        what = "eItemST_Throw_Card";
    } else {
        return false;
    }
    LOG_INFO("[ITEMEX] Player::VerifyReleaseItem : invalid %s item : %s, est:%d, itmUid:%llu, itemIndex:%u", what,
             char_name_, 20, static_cast<unsigned long long>(b.uid), d.id);
    return false;
}

void GameClientSession::handle_equip_item(PacketBuffer& pkt)
{
    try {
        if (refine_ui_) return;              // the refine / growth / reset states swallow C2S 0x51
        const uint64_t uid  = pkt.read<uint64_t>();
        const uint8_t  slot = pkt.read<uint8_t>();
        auto fail = [&]() {
            PacketBuffer p(static_cast<Opcode>(0x0053), is_paula());        // sub_5F6230
            p.write<uint64_t>(uid);
            send(p);
        };
        BagItem* b = bag_find_uid(uid);
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d) { fail(); return; }
        if (b->kind != 2) {
            // EquipItem only knows rare items (VerifyEquipItem(Rare)); a common item goes through
            // C2S 0x0205 — the retail finds no rare item here and fails.
            fail();
            return;
        }
        const int s = (slot < 23) ? static_cast<int>(slot) : -1;
        if (!verify_equip(*b, *d, s)) { fail(); return; }
        // PrepareReleaseBeforeEquipRareItem: the item in that slot comes off with it.
        std::vector<uint64_t> release;
        if (equip_[s]) {
            const BagItem* old = bag_find_uid(equip_[s]);
            const ItemDef* od = old ? server_.item_db().get(old->item_id) : nullptr;
            if (!old || !od || equip_slot_for(*od) != s) {
                LOG_INFO("[ITEMEX] Player::PrepareReleaseBeforeEquipRareItem() :Equiped item's slot is ABNORMAL : "
                         "name:%s est:%d", char_name_, s);
                fail();
                return;
            }
            release.push_back(equip_[s]);
        }
        if (equip_sync_) {                                                   // hItemEquipment.Send() duplicated
            LOG_INFO("[ITEMEX] hItemEquipment.Send() duplicated. %s, est:%d", char_name_, s);
            fail();
            return;
        }
        // hItemEquipment: the DB, then OnOk.
        equip_sync_ = true;
        for (uint64_t r : release) server_.item_store().SetEquipSlot(r, -1);
        server_.item_store().SetEquipSlot(uid, static_cast<int8_t>(s));
        equip_sync_ = false;
        equip_n_release(s, release, uid);
    } catch (...) {}
}

void GameClientSession::handle_release_item(PacketBuffer& pkt)
{
    try {
        const uint64_t uid  = pkt.read<uint64_t>();
        const uint8_t  slot = pkt.read<uint8_t>();
        auto fail = [&]() {
            PacketBuffer p(static_cast<Opcode>(0x0056), is_paula());        // sub_5F62A0
            p.write<uint64_t>(uid);
            send(p);
        };
        const BagItem* b = is_equipped(uid) ? bag_find_uid(uid) : nullptr;   // sub_6DA7F0(+2120, uid)
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d) {
            LOG_INFO("[ITEMEX] CMD_RELEASE_ITEM : not found item %s, %llu", char_name_, static_cast<unsigned long long>(uid));
            fail();
            return;
        }
        // VerifyReleaseItem (sub_763170)
        const int s = (slot < 23) ? static_cast<int>(slot) : -1;
        if (s == -1) { fail(); return; }
        if ((d->type == 17 || d->type == 18) && level_ < 20) { fail(); return; }
        if (equip_[s] && equip_[s] != uid) {
            LOG_INFO("[ITEMEX] Player::VerifyReleaseItem : invalid item : %s, est:%d, itmUid:%llu, itemIndex:%u",
                     char_name_, s, static_cast<unsigned long long>(uid), d->id);
            fail();
            return;
        }
        if (equip_sync_) { fail(); return; }
        equip_sync_ = true;
        server_.item_store().SetEquipSlot(uid, -1);
        equip_sync_ = false;
        equip_n_release(-1, { uid }, 0);
    } catch (...) {}
}

// Player::OnDie (sub_73BFA0): one equipped "unique" item of slots 0..2 may come off. The slots are
// taken in a random order (0,1,2 with three swaps against rand()%3); the first item whose flags have
// 1 and 0x100000 (sub_6DE300 -> sub_6ECD80: ItemParam Options, or a rare item's option of type 13,
// whose third value holds its own flags) and that wins rand()%5 == 0 (sub_635DE0) is released —
// Player::ReleaseItem (vtbl+300 sub_763610: VerifyReleaseItem sub_763170, then the DB release ->
// S2C 0x0055 / 0x00ED) — and dropped as an effect (sub_6046B0: S2C 855 [u16 effect object][u16 player]
// to the area — the KR client has no handler for 855, so it is not sent). Only the Shining Crystal /
// Fallen Watcher weapon, helmet and shield (and two test swords) are such items.
void GameClientSession::death_unequip()
{
    int order[3] = { 0, 1, 2 };
    for (int i = 0; i < 3; ++i) std::swap(order[i], order[std::rand() % 3]);
    for (int j = 0; j < 3; ++j) {
        const int slot = order[j];
        const uint64_t uid = equip_[slot];
        const BagItem* b = uid ? bag_find_uid(uid) : nullptr;
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!d) continue;
        uint32_t flags = static_cast<uint32_t>(d->options);
        if (d->cls == 2) {
            for (const ItemOption& o : b->opts)
                if (o.type == 13) { flags = static_cast<uint32_t>(o.extra); break; }
        }
        if (!(flags & 1) || !(flags & 0x100000)) continue;
        if (std::rand() % 5 != 0) continue;
        if (!((d->type == 17 || d->type == 18) && level_ < 20) && !equip_sync_) {   // VerifyReleaseItem
            equip_sync_ = true;
            server_.item_store().SetEquipSlot(uid, -1);
            equip_sync_ = false;
            LOG_DEBUG("GameClientSession %u: death — unique item %u (%s) comes off slot %d", session_id(), d->id,
                      d->name.c_str(), slot);
            equip_n_release(-1, { uid }, 0);
        }
        break;
    }
}

// C2S 0x0205 CMD_EQUIP_COMMON_ITEM [u64 uid][u8 slot] (Player case 517): a common item into the common
// slot 20 — bullets (Type 29: the magazine), a Throw item (Type 16) or a throw card (Type 11 SubType
// 60). Not in the bag -> S2C 0x020A [u64 uid] (the retail answers with the release's failure here);
// not a common item (Class 1) -> logged + 0x020A. Player::EquipCommonItem (vtbl+288 sub_763410):
// VerifyEquipItem(Common), the items that come off with it (sub_762510: bullets — the loaded stack
// and the throw item; a Throw item / card — sub_762640: the gun of slot 1, the loaded stack, the
// throw item), hItemEquipment (the DB; one already pending -> fail) and its OnOk EquipNReleaseItem.
// Any failure there -> S2C 0x0207 [u64 uid] (sub_5F6310).
void GameClientSession::handle_equip_common(PacketBuffer& pkt)
{
    try {
        if (refine_ui_) return;              // the refine / growth / reset states swallow C2S 0x205
        const uint64_t uid  = pkt.read<uint64_t>();
        const uint8_t  slot = pkt.read<uint8_t>();
        auto answer = [&](uint16_t op) {
            PacketBuffer p(static_cast<Opcode>(op), is_paula());
            p.write<uint64_t>(uid);
            send(p);
        };
        BagItem* b = bag_find_uid(uid);                                      // sub_60C610(+1716, uid)
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d) { answer(0x020A); return; }                            // sub_5F6380
        if (b->kind != 1) {
            LOG_INFO("CMD_EQUIP_COMMON_ITEM : item's Class Type is not Common - ITMUID:%llu, ID:%u, ChrUid:%u",
                     static_cast<unsigned long long>(uid), b->item_id, char_uid_);
            answer(0x020A);
            return;
        }
        // Hardening (not retail): the common slot is 20 (what the client sends); another byte was stored as the item's
        // equip_slot, and the next login put the bullets / throw item into that real equipment slot.
        if (slot != 20) { answer(0x0207); return; }
        const int s = slot;
        std::vector<uint64_t> release;
        bool ok = verify_equip_common(*b, *d, s);
        if (ok && d->type == 29) {
            if (mag_.item)   release.push_back(mag_.item);
            if (throw_item_) release.push_back(throw_item_);
        } else if (ok && (d->type == 16 || d->type == 11)) {
            if (d->type == 11 && d->sub_type != 60) {
                LOG_INFO("[ITEMEX] Abnormal trying to Equip Card : %s, equipItem : %u, %llu", char_name_, d->id,
                         static_cast<unsigned long long>(uid));
                ok = false;
            } else {
                if (gun_in_any_slot()) {                                     // sub_762640
                    if (equip_[1]) release.push_back(equip_[1]);
                    else ok = false;
                }
                if (mag_.item)   release.push_back(mag_.item);
                if (throw_item_) release.push_back(throw_item_);
            }
        }
        if (ok && equip_sync_) {
            LOG_INFO("[ITEMEX] hItemEquipent.Send() duplicated. %s, est:%d, equipUid:%llu", char_name_, s,
                     static_cast<unsigned long long>(uid));
            ok = false;
        }
        if (!ok) { answer(0x0207); return; }
        // hItemEquipment: the DB (item_pos = the slot), then OnOk.
        equip_sync_ = true;
        for (uint64_t r : release) server_.item_store().SetEquipSlot(r, -1);
        server_.item_store().SetEquipSlot(uid, static_cast<int8_t>(s));
        equip_sync_ = false;
        equip_n_release(s, release, uid);
    } catch (...) {}
}

// C2S 0x0208 CMD_RELEASE_COMMON_ITEM [u64 uid][u8 slot] (Player case 520, client sub_8CF190 -> c2s_520):
// not in the bag / not a common item -> S2C 0x020A [u64 uid]; Player::ReleaseCommonItem (vtbl+296
// sub_7635B0): VerifyReleaseItem (sub_762EF0), the DB, EquipNReleaseItem with only that item
// (-> S2C 0x0209). Any failure -> 0x020A (sub_5F6380).
void GameClientSession::handle_release_common(PacketBuffer& pkt)
{
    try {
        const uint64_t uid  = pkt.read<uint64_t>();
        const uint8_t  slot = pkt.read<uint8_t>();
        auto fail = [&]() {
            PacketBuffer p(static_cast<Opcode>(0x020A), is_paula());
            p.write<uint64_t>(uid);
            send(p);
        };
        const BagItem* b = bag_find_uid(uid);
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d) { fail(); return; }
        if (b->kind != 1) {
            LOG_INFO("CMD_EQUIP_COMMON_ITEM : item's Class Type is not Common - ITMUID:%llu, ID:%u, ChrUid:%u",
                     static_cast<unsigned long long>(uid), b->item_id, char_uid_);
            fail();
            return;
        }
        if (!verify_release_common(*b, *d, slot)) { fail(); return; }
        if (equip_sync_) { fail(); return; }                                 // hItemEquipent.Send() duplicated
        equip_sync_ = true;
        server_.item_store().SetEquipSlot(uid, -1);
        equip_sync_ = false;
        equip_n_release(-1, { uid }, 0);
    } catch (...) {}
}

// The gun's equip effects (Player vtbl+216 sub_7605A0 -> sub_717E70 / vtbl+220 sub_760600): equipping
// one switches the player's strategies to the gun's (5 / 1) and drops the running action
// (ChangeState 0: the attack state is left — S2C 0x0020 — and so is a pick-up); taking it off
// switches them back (1 / 2), fresh from their Init (idle): an attack that was running stays in its
// state but does nothing more. The ranges follow slot 1 (attack_range).
void GameClientSession::gun_equip_changed(bool gun_in, bool gun_out)
{
    if (gun_out && attacking_) {
        pf_ = BattleFollow{};
        gun_start_ = gun_stop_ = false;
    }
    if (gun_in) {
        end_attack(true);
        pick_ = Pickup{};
    }
}

// EquipNReleaseItem (sub_763FD0) after hItemEquipment::OnOk.
void GameClientSession::equip_n_release(int slot, const std::vector<uint64_t>& release, uint64_t equip_uid)
{
    const uint16_t self = static_cast<uint16_t>(object_id_);
    bool gun_in = false, gun_out = false;
    // ReleaseItems (sub_7639C0): a rare item leaves the first slot holding an item of that id
    // (sub_6DA5A0); a common item the common slot 20 (sub_79E5E0 -> vtbl+340 sub_763900: the magazine
    // empties when it holds bullets of that id, the throw slot when it holds a Throw item / throw card)
    // -> S2C 0x0209 [u64 uid][u8 20] (sub_5F61B0).
    for (uint64_t r : release) {
        const BagItem* b = bag_find_uid(r);
        if (!b) {
            LOG_INFO("[ITEMEX] Player::ReleaseItems failed: %s, itmUid:%llu", char_name_, static_cast<unsigned long long>(r));
            continue;
        }
        const ItemDef* rd = server_.item_db().get(b->item_id);
        if (b->kind == 1) {
            if (rd && rd->type == 29 && mag_.item) {
                const BagItem* loaded = bag_find_uid(mag_.item);
                if (loaded && loaded->item_id == b->item_id) mag_ = Magazine{};
            }
            if (rd && (rd->type == 16 || (rd->type == 11 && rd->sub_type == 60))) throw_item_ = 0;
            PacketBuffer own(static_cast<Opcode>(0x0209), is_paula());
            own.write<uint64_t>(r);
            own.write<uint8_t>(20);
            send(own);
            continue;
        }
        if (b->kind != 2) continue;
        if (rd && rd->sub_type == 54) gun_out = true;                       // sub_760600
        int s = -1;
        for (int i = 0; i < 23 && s < 0; ++i) {
            if (!equip_[i]) continue;
            const BagItem* e = bag_find_uid(equip_[i]);
            if (e && e->item_id == b->item_id) s = i;
        }
        if (rd && rd->type == 13) pet_released();                           // PetItemHelper::OnRelease (pet.cpp)
        if (s >= 0) equip_[s] = 0;                                           // sub_7638C0 -> sub_6DA480
        PacketBuffer own(static_cast<Opcode>(0x0055), is_paula());
        own.write<uint64_t>(r);
        own.write<uint8_t>(static_cast<uint8_t>(s));
        send(own);
        PacketBuffer area(static_cast<Opcode>(0x00ED), is_paula());
        area.write<uint16_t>(self);
        area.write<uint32_t>(b->item_id);
        server_.broadcast_zone(zone_id_, area, 0);
    }
    if (equip_uid) {
        const BagItem* b = bag_find_uid(equip_uid);
        const ItemDef* ed = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b) {
            LOG_INFO("Player::EquipNReleaseItem failed: %s, est:%d, itmUid:%llu", char_name_, slot,
                     static_cast<unsigned long long>(equip_uid));
            PacketBuffer p(static_cast<Opcode>(0x0053), is_paula());
            p.write<uint64_t>(equip_uid);
            send(p);
        } else if (b->kind == 1) {
            // A common item (vtbl+332 sub_763F10): bullets load the magazine {1, count, stack} and clear
            // the throw slot; a Throw item / throw card takes the throw slot. S2C 0x0206 [u64 uid][u8 slot]
            // (sub_5F6130; the client loads its gun / throw slot, sub_6AE610).
            if (ed && ed->type == 29) {
                mag_.per_shot = 1;
                mag_.loaded   = b->count;
                mag_.item     = b->uid;
                throw_item_   = 0;
            } else if (ed && (ed->type == 16 || (ed->type == 11 && ed->sub_type == 60))) {
                throw_item_ = b->uid;
            }
            PacketBuffer own(static_cast<Opcode>(0x0206), is_paula());
            own.write<uint64_t>(equip_uid);
            own.write<uint8_t>(static_cast<uint8_t>(slot));
            send(own);
        } else if (b->kind == 2) {
            if (ed && ed->sub_type == 54) gun_in = true;                    // sub_7605A0 -> sub_717E70
            if (ed && weapon_helper_type(ed->type)) last_shot_tick_ = tick_ms();   // sub_6E70F0 -> sub_765480
            if (ed && ed->type == 6) cdm_on_equip_drill(*b);                 // DrillItemHelper sub_6E6EF0
            equip_[slot] = equip_uid;                                        // sub_6DA430
            if (ed && ed->type == 13) pet_equipped(*b, *ed);                 // PetItemHelper::OnEquip: op 9 (pet.cpp)
            PacketBuffer own(static_cast<Opcode>(0x0052), is_paula());
            own.write<uint64_t>(equip_uid);
            own.write<uint8_t>(static_cast<uint8_t>(slot));
            send(own);
            PacketBuffer area(static_cast<Opcode>(0x00EC), is_paula());
            area.write<uint16_t>(self);
            area.write<uint32_t>(b->item_id);
            area.write<uint32_t>(b->item_id);                               // the shown item (a look option, type 12: none)
            server_.broadcast_zone(zone_id_, area, 0);
        }
    }
    recompute_stats();                                                       // vtbl+160: the item's stats in/out
    gun_equip_changed(gun_in, gun_out);
    LOG_DEBUG("GameClientSession %u: equipment changed (slot %d, released %zu, equipped %llu) -> AP=%d AC=%d DX=%d "
              "DP=%d HV=%d HP=%d MP=%d WT=%d", session_id(), slot, release.size(),
              static_cast<unsigned long long>(equip_uid), st_ap_, st_ac_, st_dx_, st_dp_, st_hv_, hp_max_, mp_max_,
              st_maxwt_);
}

// The two equipment masks of the player appear (sub_6DA070 + sub_73AF20; client sub_58CA90 /
// sub_6C8550): [u32 mask][u32 item id per bit] for the visible slots in this order — 0, 1, 2, 5, 6,
// 10..19 (bits 0x80..0x10000), 7 (0x40000), 21 (0x80000) — then the look-option mask (type 12) for
// slots 0, 1, 2, 15, 16, 21 (none here: 0).
void GameClientSession::write_equip_appearance(PacketBuffer& p) const
{
    static constexpr struct { int slot; uint32_t bit; } kShown[] = {
        { 0, 0x1 }, { 1, 0x2 }, { 2, 0x4 }, { 5, 0x20 }, { 6, 0x40 }, { 10, 0x80 }, { 11, 0x100 }, { 12, 0x200 },
        { 13, 0x400 }, { 14, 0x800 }, { 15, 0x1000 }, { 16, 0x2000 }, { 17, 0x4000 }, { 18, 0x8000 },
        { 19, 0x10000 }, { 7, 0x40000 }, { 21, 0x80000 } };
    uint32_t mask = 0;
    std::vector<uint32_t> ids;
    for (const auto& e : kShown) {
        if (!equip_[e.slot]) continue;
        for (const BagItem& b : bag_)
            if (b.uid == equip_[e.slot]) { mask |= e.bit; ids.push_back(b.item_id); break; }
    }
    p.write<uint32_t>(mask);
    for (uint32_t id : ids) p.write<uint32_t>(id);
    p.write<uint32_t>(0);
}
