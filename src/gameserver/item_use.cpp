#include "game_session.hpp"
#include "game_server.hpp"
#include "item_wire.hpp"
#include "client_text.hpp"
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  Using an item — retail GameServer, verified against the KR client.
//
//  C2S 0x0057 [u64 uid] (Player state machine sub_73D2A0 case 87, Normal state):
//    a pending skill selection (+0x148 flag 1) is cancelled first (sub_75F790: S2C 243);
//    no such item in the bag -> S2C 0x005A (sub_5F6730, empty); the player's condition flags
//    (+1636 0x80000000, sub_41C6B0 4 / 0x400 (Type 10 only) / 0x80000 — no condition exists here
//    yet) -> S2C 0x005A;
//    ItemParam Delay > 0 -> the running action stops and nothing else (those items — Type 11
//    SubType 33 "Spirit" cards — are used on a target, C2S 0x0058 / Player state 7);
//    else Player::UseItem (vtbl+144 sub_75D850); false -> S2C 0x005A.
//  Player::UseItem: Usable (Options 2), level >= MinLevel, the item exists, then the helper of the
//    item's Type (sub_6DFA70, table dword_AB617C: 1 Money, 10 Potion, 11 Card, 13 Pet, 14 Misc,
//    15 UseItem, 16 Throw, 6 Drill, 28 UseSkillCard, 29 Bullet, 2 Weapon, 3-5/7/8/17-27/34 General):
//      1 -> UseItem takes one (sub_79DD20) unless OptionsEx 64 ("permanent");
//      2 -> nothing more;
//      3 -> the helper already took one; UpdateDelayedItem (sub_765730) saves the amount later;
//      0 -> fail.
//    an Options 0x4000 item only for the character types in its ChrTypeFlags.
//    (Its other checks belong to items whose helpers are not here yet: OptionsEx 0x200 with
//    sub_76A9B0; the effects 15, 21, 23-26 without the player's +1060 flag 1 -> S2C 0x01E4 [u8 8]
//    [u16 0][u16 0].)
//  The client takes the item off its window itself — on S2C 0x0059 [u64 uid] (sub_6AE3B0) or on the
//  item's effect block (S2C 0x0097, sub_6AD940); a use sends no bag record.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
constexpr int64_t kMaxMoney = 2100000000;   // sub_765960: 0x7D2B7500
}

void GameClientSession::handle_use_item(PacketBuffer& pkt)
{
    auto refuse = [&]() {
        PacketBuffer p(static_cast<Opcode>(0x005A), is_paula());
        send(p);
    };
    es_on_item_use();                                          // the Player's case 87: our ESs first (vtbl+72)
    try {
        if (su_.selecting) {                                   // (+0x148).HasFlag(1) -> sub_75F790
            skill_cancel_using();
            su_.selecting = false;
        }
        const uint64_t uid = pkt.read<uint64_t>();
        BagItem* b = bag_find_uid(uid);                        // sub_60C610(+1716, uid)
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d) { refuse(); return; }
        if (d->delay > 0.0f) {                                 // +100 > 0.0: not used through 0x0057
            end_attack(true);                                  // sub_41C5D0
            return;
        }
        if (!use_item(*b, *d)) refuse();
    } catch (...) {}
}

// Player::UseItem (sub_75D850). `b` may be gone afterwards (the last one used up).
bool GameClientSession::use_item(BagItem& b, const ItemDef& d)
{
    if (!(d.options & 2)) return false;                       // Usable (sub_6DE120)
    if (level_ < d.min_level) return false;                   // m_Level < ItemMinLevel
    // an Options 0x4000 item only for the character types of its ChrTypeFlags (sub_6DE1A0 / sub_6DE080:
    // bit type-1 of ItemParam +60; Player +436 = the character type, 1..24)
    if ((d.options & 0x4000) && !(type_ >= 1 && type_ < 25 && (d.chr_type_flags & (1u << (type_ - 1)))))
        return false;
    const uint64_t uid = b.uid;
    const uint32_t id  = b.item_id;
    switch (use_item_helper(b, d)) {
    case 1:
        if (d.options_ex & 64) {
            LOG_DEBUG("Item count didn't deacrease by Permanent OptionEx ( id:%u )", id);
        } else if (BagItem* it = bag_find_uid(uid)) {          // sub_79DD20(this, item, 1)
            const BagRecord rec = bag_decrease(*it, 1);
            if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
            else               server_.item_store().SetAmount(rec.uid, rec.count);
            bag_apply(rec);
        }
        return true;
    case 2:
        return true;
    case 3:
        if (d.options_ex & 64) {
            LOG_DEBUG("Item count didn't deacrease by Permanent OptionEx ( id:%u )", id);
        } else if (BagItem* it = bag_find_uid(uid)) {          // UpdateDelayedItem: the new amount
            server_.item_store().SetAmount(it->uid, it->count);
        }
        return true;
    default:
        return false;
    }
}

// sub_6DFA70: the helper of the item's Type (dword_AB617C, Types 1..35) runs Use; an item without
// one gets S2C 0x0059 [u64 uid] (sub_5F66C0) and is used up.
int GameClientSession::use_item_helper(BagItem& b, const ItemDef& d)
{
    const uint16_t self = static_cast<uint16_t>(object_id_);
    switch (d.type) {
    case 10:
    case 29: {
        // PotionItemHelper::Use (sub_6E6120): an HP potion needs HP below the max and an MP potion
        // MP below the max (sub_6E6270 / sub_6E62B0 — both, for one that heals both).
        // BulletItemHelper::Use (sub_6E69F0) is the same use (its effect bytes sub_6E6AB0 are the
        // potion's), but it takes nothing: 3, the amount stays.
        if (!(d.hp <= 0 || hp_ < hp_max_) || !(d.mp <= 0 || mp_ < mp_max_)) return 0;
        std::vector<PacketBuffer> after;
        if (d.hp) {                                            // sub_6E62F0 -> S2C 0x0024 [u32 hp]
            int32_t v = hp_ + d.hp;
            if (v <= hp_max_) { if (v < 0) v = 0; } else v = hp_max_;
            hp_ = v;
            PacketBuffer p(static_cast<Opcode>(0x0024), is_paula());
            p.write<uint32_t>(static_cast<uint32_t>(hp_));
            after.push_back(std::move(p));
        }
        if (d.mp) {                                            // sub_6E6370 -> S2C 0x0025 [u32 mp]
            int32_t v = mp_ + d.mp;
            if (v <= mp_max_) { if (v < 0) v = 0; } else v = mp_max_;
            mp_ = v;
            PacketBuffer p(static_cast<Opcode>(0x0025), is_paula());
            p.write<uint32_t>(static_cast<uint32_t>(mp_));
            after.push_back(std::move(p));
        }
        // sub_6E63F0: [u16 Effect][u16 user]
        const std::vector<uint8_t> eff = { static_cast<uint8_t>(d.effect & 0xFF), static_cast<uint8_t>((d.effect >> 8) & 0xFF),
                                           static_cast<uint8_t>(self & 0xFF), static_cast<uint8_t>(self >> 8) };
        item_effect_block(b, d, eff, std::move(after));
        LOG_DEBUG("GameClientSession %u: used %u (%s): HP %d/%d MP %d/%d", session_id(), d.id, d.name.c_str(),
                  hp_, hp_max_, mp_, mp_max_);
        if (d.type == 29) return 3;
        if (b.count == 1) return 1;
        bag_apply(bag_decrease(b, 1));                         // sub_6E4460 + sub_6DBA50
        return 3;
    }
    case 1: {
        // MoneyItemHelper::Use (sub_6E18A0): the item's Money joins ours within 0..2,100,000,000
        // (else S2C 0x005A — and UseItem fails: a second 0x005A, retail).
        const int64_t total = static_cast<int64_t>(money_) + d.money;
        if (total < 0 || total > kMaxMoney) {
            PacketBuffer p(static_cast<Opcode>(0x005A), is_paula());
            send(p);
            return 0;
        }
        money_ = static_cast<int32_t>(total);                  // sub_73B280
        PacketBuffer m(static_cast<Opcode>(0x0029), is_paula());   // sub_5F4E80 [u32 money]
        m.write<uint32_t>(static_cast<uint32_t>(money_));
        std::vector<PacketBuffer> after;
        after.push_back(std::move(m));
        const std::vector<uint8_t> eff = { static_cast<uint8_t>(d.effect & 0xFF), static_cast<uint8_t>((d.effect >> 8) & 0xFF),
                                           static_cast<uint8_t>(self & 0xFF), static_cast<uint8_t>(self >> 8) };
        item_effect_block(b, d, eff, std::move(after));
        save_position();                                       // sub_5EF5D0: the money to the DB (3619)
        LOG_DEBUG("GameClientSession %u: used %u (%s): money %d", session_id(), d.id, d.name.c_str(), money_);
        return 1;
    }
    case 14: {
        // MiscItemHelper::Use (sub_6E19E0): only the user sees it — S2C 0x0097 [u64 uid][u16 Effect]
        // (sub_5F6470), in a block of its own.
        PacketBuffer p(static_cast<Opcode>(0x0097), is_paula());
        p.write<uint64_t>(b.uid);
        p.write<uint16_t>(static_cast<uint16_t>(d.effect));
        std::vector<PacketBuffer> mine, theirs;
        mine.push_back(std::move(p));
        server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
        return 1;
    }
    case 28:
        return use_skill_card(b, d);
    case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 13: case 17: case 18:
    case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 34:
        // Weapon/General/Drill/PetItemHelper: their Use (sub_6E6B00) returns 0 — not a use.
        return 0;
    case 15:
        return use_item_helper_15(b, d);
    case 11: case 16:
        // CardItemHelper / ThrowItemHelper.
        LOG_INFO("GameClientSession %u: item %u (%s) Type %d — its use helper is not implemented yet",
                 session_id(), d.id, d.name.c_str(), d.type);
        return 0;
    default: {
        PacketBuffer p(static_cast<Opcode>(0x0059), is_paula());
        p.write<uint64_t>(b.uid);
        send(p);
        return 1;
    }
    }
}

// UseItemHelper::Use (sub_6E1AE0), the Type 15 items, in its order:
//   Effect 43 / 44 (treasure maps, sub_76AFC0)                                    — not here yet;
//   SubType 72 / 74 / 69 / 75 / 77 / 71 (seeds, soul shards, the skill scrolls and seals, the soul
//     ticket, rune stones, disguise kits)                       — 69 below, the others not here yet;
//   the item's flags (a rare item's type-13 option, else ItemParam Options) with 0x8000 — the items the
//     client sends a destination / text with (Player +2936): Effect 14 / 20 / 34 / 39 (ports), 15 (hair
//     dye), 21, 23, 24, 25, 31 (rename, use_rename_item), 32 (the TBN Megaphone, megaphone.cpp), 34 (the
//     Memory Port, memory_port.cpp)                                                 — 21 not yet;
//   then by Effect: 19 (go to the return town), 26 (status reset), 28 (brooms), 37 (Star Tears), 46
//     (Fiesta ticket) — not here yet; 22 gift boxes, 27 warehouse slots, 29 rebirth — below;
//   any other Effect: the generic use — S2C 151 [u64 uid][u16 Effect] in a command block of its own
//     (sub_6E56B0 + sub_6E1A50) and the item is used up (1). (The recovered EXE has no case for the
//     newer data's Effects 47..51: they take this path too.)
//   The ones not here yet answer 0 (the use is refused, S2C 0x5A) rather than use the item up.
int GameClientSession::use_item_helper_15(BagItem& b, const ItemDef& d)
{
    auto todo = [&](const char* what) {
        LOG_INFO("GameClientSession %u: item %u (%s) Type 15 SubType %d Effect %d (%s) — not implemented yet",
                 session_id(), d.id, d.name.c_str(), d.sub_type, d.effect, what);
        return 0;
    };
    if (d.effect == 43 || d.effect == 44) return todo("treasure map");
    switch (d.sub_type) {
    case 72: return todo("seed");
    case 74: return todo("soul shard");
    case 69: return use_scroll_item(b, d);
    case 75: return todo("guardian slot");
    case 77: return todo("rune stone");
    case 71: return use_disguise_kit(b, d);
    default: break;
    }
    uint32_t flags = static_cast<uint32_t>(d.options);         // sub_6ECC40
    if (b.kind == 2)
        for (const ItemOption& o : b.opts) if (o.type == 13) { flags = static_cast<uint32_t>(o.extra); break; }
    if (flags & 0x8000) {
        switch (d.effect) {
        case 14: case 20: case 39: return use_port_item(b, d, true);
        case 34: return use_memory_port(b, d);
        case 15: return use_hair_dye(b, d);
        case 24: return use_reset_status(b, d, false);
        case 25: return use_reset_graph(b, d);
        case 23: return use_reset_skill(b, d);
        case 21: return use_char_type_change(b, d);
        case 31: return use_rename_item(b, d);
        case 32: return use_megaphone_booster(b, d);
        default: break;
        }
    }
    switch (d.effect) {
    case 22: return use_present_item(b, d);
    case 27: return use_warehouse_slot_item(b, d);
    case 29: return use_rebirth_item(b, d);
    case 19: return use_port_item(b, d, false);
    case 26: return use_reset_status(b, d, true);
    case 28: return use_port_item(b, d, false);
    case 37: return todo("star tears");
    case 46: return todo("fiesta ticket");
    default: break;
    }
    PacketBuffer p(static_cast<Opcode>(0x0097), is_paula());
    p.write<uint64_t>(b.uid);
    p.write<uint16_t>(static_cast<uint16_t>(d.effect));
    std::vector<PacketBuffer> mine, theirs;
    mine.push_back(std::move(p));
    server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    LOG_DEBUG("GameClientSession %u: used %u (%s), Type 15 Effect %d — the generic use", session_id(), d.id,
              d.name.c_str(), d.effect);
    return 1;
}

// UseItemHelper::Use case 22 — a gift box (PresentItemParam2): the bag's weight under MaxWT (else S2C 591
// [u16 1] "OverLimitedWt" and 0); its items (GeneratePresentItems sub_6DFD80: no row -> 0): DropCnt picks
// (Type 0: by DropRate, rand() % their sum, sub_6DFF50; 1: the same up to 32 times for an item not
// listed yet; 2: slot i), a common item already listed stacks onto its line; hPresentItem (sub_6E3F60):
// no line or an item id 0 -> 0; fewer free bag entries than lines -> S2C 591 [u16 2] "FullInventory";
// the add records (sub_6DC000 -> sub_6DBD10: onto the bag's stack of the id (1), else new (4) with a rare
// item's default stats, sub_6E0490); the DB (type 25) -> OnOk (sub_7819A0): the records applied, S2C 590
// [u32 the box][u16 n]{record} (sub_5FF390; client sub_6F0960 case 590). The use itself: S2C 151 [u64
// uid][u16 22] in a command block of its own, the box used up (1). (Only the bag's weight before is
// checked, not the contents'.) VERIFIED in the client: cases 590 / 591 (1 / 2 / 3 UnknownError).
int GameClientSession::use_present_item(BagItem& b, const ItemDef& d)
{
    auto refuse = [&](uint16_t why) {
        PacketBuffer p(static_cast<Opcode>(0x024F), is_paula());       // S2C 591 (sub_5FF440)
        p.write<uint16_t>(why);
        send(p);
    };
    if (!(st_maxwt_ > bag_weight_)) { refuse(1); return 0; }
    const PresentItem* box = server_.present_item_db().get(d.id);
    if (!box) {
        LOG_ERROR("GeneratePresentItems - not found presentItem, itemId:%u", d.id);
        return 0;
    }
    // GeneratePresentItems (sub_6DFD80)
    struct Line { uint32_t item; int cnt; };
    std::vector<Line> lines;
    auto pick = [&]() -> size_t {                                       // sub_6DFF50
        if (box->rate_sum <= 0) return 0;                               // (rand() % 0 in the retail)
        const int r = std::rand() % box->rate_sum;
        int acc = 0;
        for (size_t i = 0; i < box->slots.size(); ++i) {
            acc += box->slots[i].rate;
            if (acc > r) return i;
        }
        return 0;
    };
    auto listed = [&](uint32_t item) -> int {                           // sub_6DFFC0
        for (size_t i = 0; i < lines.size(); ++i) if (lines[i].item == item) return static_cast<int>(i);
        return -1;
    };
    for (int i = 0; i < box->drop_cnt; ++i) {
        size_t k = 0;
        if (box->type == 0) {
            k = pick();
        } else if (box->type == 1) {
            for (int j = 0; j < 32; ++j) {
                k = pick();
                if (k < box->slots.size() && listed(box->slots[k].item) == -1) break;
            }
        } else if (box->type == 2) {
            k = static_cast<size_t>(i);
        }
        if (k >= box->slots.size()) continue;                          // (the retail reads past its slots)
        const PresentItem::Slot& s = box->slots[k];
        const ItemDef* idf = server_.item_db().get(s.item);
        if (!idf) {
            LOG_ERROR("GeneratePresentItems - box %u: item %u is not in ItemParam", d.id, s.item);
            continue;
        }
        const int at = (idf->cls == 1) ? listed(s.item) : -1;
        if (at < 0) lines.push_back({ s.item, s.cnt });
        else        lines[static_cast<size_t>(at)].cnt += s.cnt;
    }
    // hPresentItem::Send (sub_6E3F60)
    if (lines.empty()) {
        LOG_INFO("hPresentItem::Send - CHRUID:%u, cnt <= 0: %u,%s", char_uid_, static_cast<unsigned>(object_id_),
                 char_name_);
        return 0;
    }
    for (const Line& l : lines) if (!l.item) return 0;
    if (300 - static_cast<int>(bag_.size()) < static_cast<int>(lines.size())) { refuse(2); return 0; }
    const uint64_t uid = b.uid;                                         // (`b` may move with the bag)
    std::vector<QuestItem> adds;
    for (const Line& l : lines) adds.push_back({ l.item, l.cnt });
    // the use's effect (at once), then the DB's answer: the records, S2C 590
    {
        PacketBuffer p(static_cast<Opcode>(0x0097), is_paula());
        p.write<uint64_t>(uid);
        p.write<uint16_t>(static_cast<uint16_t>(d.effect));
        std::vector<PacketBuffer> mine, theirs;
        mine.push_back(std::move(p));
        server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    }
    const std::vector<BagRecord> applied = quest_bag_records({}, adds);
    PacketBuffer r(static_cast<Opcode>(0x024E), is_paula());           // S2C 590
    r.write<uint32_t>(d.id);
    r.write<uint16_t>(static_cast<uint16_t>(applied.size()));
    for (const BagRecord& rec : applied) write_bag_record(r, rec);
    send(r);
    LOG_DEBUG("%s : hPresentItem:: Give (box %u: %zu lines, %zu records)", char_name_, d.id, lines.size(),
              applied.size());
    return 1;
}

// UseItemHelper::Use SubType 69 (sub_6E59C0) — a skill scroll / seal: its SkillScroll_Info row (none -> 0);
// the row's skill of a debuff class (sub_894280: ESClass 4 / 5 / 6) and >= 6000, else 0; a passive one
// (Type 1, a seal) would be learned at level 1 (not learned yet + Player::CanLearnSkill, S2C 0x020B /
// 0x01DF) — no row of this data names a passive skill, so that branch is not done; then S2C 0x0059
// [u64 uid] in a command block of its own (sub_6E1590 -> sub_5F66C0) and one is used up (1). Reached when
// a scroll's effect state is made (es.cpp scroll_use_up) — and by a plain C2S 0x0057 on a scroll, which
// so uses it up for nothing (retail; the client uses C2S 0x034C).
int GameClientSession::use_scroll_item(BagItem& b, const ItemDef& d)
{
    const SkillScrollDef* row = server_.skill_scroll_db().get(d.id);
    if (!row) return 0;
    const SkillDef* s = server_.skill_db().get(row->skill);
    if (!s) return 0;                                          // (a null read in the retail)
    if (!(s->esclass == 4 || s->esclass == 5 || s->esclass == 6) || s->id < 6000) return 0;
    if (s->type == 1) {
        LOG_INFO("GameClientSession %u: seal %u (%s) of the passive skill %d — not implemented", session_id(), d.id,
                 d.name.c_str(), s->id);
        return 0;
    }
    PacketBuffer p(static_cast<Opcode>(0x0059), is_paula());
    p.write<uint64_t>(b.uid);
    std::vector<PacketBuffer> mine, theirs;
    mine.push_back(std::move(p));
    server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    LOG_DEBUG("GameClientSession %u: skill scroll %u (%s) used up (skill %d)", session_id(), d.id, d.name.c_str(), s->id);
    return 1;
}

// UseSkilCardlHelper::Use (sub_6E7460): a skill card (Type 28 SubType 55, Item_SkillCardParam) teaches
// its AffectionSkill at RegisterLevel for AffectionPoint TM points: a skill already learned -> S2C
// 0x01E4 [5][skill][1]; Player::CanLearnSkill (its own messages); then the TM points go, the learn
// (S2C 0x020B + 0x01DF, the list, the DB) and the user's own effect block — S2C 0x0097 [u64 uid]
// [u16 Effect] (sub_6E75F0 / sub_6E1A50) — and the card is used up (1).
int GameClientSession::use_skill_card(BagItem& b, const ItemDef& d)
{
    if (d.type != 28 || d.sub_type != 55) return 0;
    const SkillCardDef* c = server_.skill_card_db().get(d.id);
    if (!c) return 0;
    if (find_skill(c->affection_skill)) {
        send_system_msg(5, static_cast<uint16_t>(c->affection_skill), 1);
        return 0;
    }
    if (!can_learn_skill(c->affection_skill)) return 0;
    tm_points_ -= c->affection_point;                          // Player +4592
    if (!learn_skill(c->affection_skill, c->register_level, c->affection_point)) return 0;
    save_stat_points();                                        // the TM points with the stat points
    PacketBuffer p(static_cast<Opcode>(0x0097), is_paula());
    p.write<uint64_t>(b.uid);
    p.write<uint16_t>(static_cast<uint16_t>(d.effect));
    std::vector<PacketBuffer> mine, theirs;
    mine.push_back(std::move(p));
    server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    return 1;
}

// UseItemHelper::Use (sub_6E1AE0) case 29 — a resurrection item: its Item_RebirthParam row (none -> 0);
// not in a Battlefield / SoccerArena / DuelField map (MapInfoEx Type 7/8/9) nor in one whose Option
// has 32; the revive (sub_764260) with the row's ratios of the selected player (+2696) or else of the
// user -> the DB record (sub_7641C0) and S2C 0x02F8 [u16 revived][u16 user] to the area (sub_601C90;
// client: the "eft_item_rebirth" effect + "msg_rebirth_by_myself") -> 1. A selected object that is no
// player or cannot be revived -> S2C 0x02F9 (__fputchar_113) and 0; the user who cannot be (not dead,
// already reviving) -> 0. The helper sends no effect, so the dispatcher (sub_6DFA70) answers the use
// with S2C 0x0059 [u64 uid] (sub_6E1590 -> sub_5F66C0): the client takes one off its window; then
// UseItem takes one (sub_79DD20). Nothing selects a player here (+2696 stays empty — the death clears
// it, sub_73BFA0), so the user is the one revived.
int GameClientSession::use_rebirth_item(BagItem& b, const ItemDef& d)
{
    const ItemRebirthDef* r = server_.item_rebirth_db().get(d.id);
    if (!r) return 0;
    if (const MapDef* z = server_.maps_db().get(zone_id_)) {
        if (z->type == 8 || z->type == 9 || z->type == 7) return 0;
        if (z->option & 32) return 0;
    }
    if (!revive(r->hp_ratio, r->mp_ratio, r->exp_ratio, r->tmxp_ratio)) return 0;
    const uint16_t self = static_cast<uint16_t>(object_id_);
    PacketBuffer p(static_cast<Opcode>(0x02F8), is_paula());
    p.write<uint16_t>(self);
    p.write<uint16_t>(self);
    server_.broadcast_zone(zone_id_, p, 0);
    PacketBuffer u(static_cast<Opcode>(0x0059), is_paula());
    u.write<uint64_t>(b.uid);
    send(u);
    server_.char_store().SaveState(char_uid_, zone_id_, pos_x_, pos_y_, money_, level_, exp_, hp_, mp_);
    LOG_DEBUG("GameClientSession %u: Resurrect with %u (%s) zone=%d hp=%d/%d mp=%d/%d", session_id(), d.id,
              d.name.c_str(), zone_id_, hp_, hp_max_, mp_, mp_max_);
    return 1;
}

// ── C2S 0x0108: use an item with custom data (Player case 264, CMD_USE_ITEM_CUSTOM_DATA) ──────────────
//  [u64 uid][i16 n][n bytes]. Retail: a selection still open is cancelled (sub_75F790: S2C 243); the
//  player's +1636 flag 0x80000000 -> S2C 0x005A; Player +2936 is emptied and, n in 0..256, filled with the
//  bytes (else the log "Msg( CMD_USE_ITEM_CUSTOM_DATA )" and S2C 0x005A); the item by uid; the conditions
//  4 / 0x400 (not a Type 10 item) -> S2C 0x005A (none exists here yet); no item or its HPCon / MPCon not met
//  (sub_73C940) -> S2C 0x005A; +0xC94 = the item; ItemParam Delay > 0 -> the target state 7 (not here, as
//  C2S 0x0057); else Player::UseItem — false -> S2C 0x005A. The Options 0x8000 items read the data
//  (UseItemHelper::Use, below).
void GameClientSession::handle_use_item_custom(PacketBuffer& pkt)
{
    auto refuse = [&]() {
        PacketBuffer p(static_cast<Opcode>(0x005A), is_paula());
        send(p);
    };
    try {
        if (su_.selecting) {                                   // (+0x148).HasFlag(1) -> sub_75F790
            skill_cancel_using();
            su_.selecting = false;
        }
        const uint64_t uid = pkt.read<uint64_t>();
        const int16_t  n   = pkt.read<int16_t>();
        use_data_ = UseData{};                                 // sub_4039E0
        if (n < 0 || n > 256) {
            LOG_ERROR("%s : Msg( CMD_USE_ITEM_CUSTOM_DATA ) : invalid size %d", char_name_, n);
            refuse();
            return;
        }
        for (int16_t i = 0; i < n; ++i) use_data_.b.push_back(pkt.read<uint8_t>());   // sub_43AFF0
        BagItem* b = bag_find_uid(uid);
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d || hp_ < d->hp_con || mp_ < d->mp_con) { refuse(); return; }
        scroll_uid_ = uid;                                     // +0xC94
        LOG_DEBUG("GameClientSession %u: C2S 0x0108 use %u (%s) with %d bytes", session_id(), d->id, d->name.c_str(), n);
        if (d->delay > 0.0f) {                                 // the target state 7 (a card) — not here yet
            end_attack(true);
            return;
        }
        if (!use_item(*b, *d)) refuse();
    } catch (...) {}
}

// MapInfoEx Option 128: no item port out of (or, Option 1024, into — Friend Finder) the zone.
bool GameClientSession::zone_no_port(int zone)
{
    const MapDef* z = server_.maps_db().get(zone);
    return z && (z->option & 128);
}

// UseItemHelper::Use's port (sub_6E1AE0), in one command block (sub_41C6F0 .. sub_41C700): S2C 151 [u64 uid]
// [u16 Effect] to the user (sub_6E56B0 + sub_6E1A50 -> sub_5F6470), the destination (Player +3128 zone,
// +3132 point, sub_50D7E0), the zone change asked for 200 ms later (sub_50D810: event 30083 [3][zone][point]
// [0.0] -> zone sub_7FDCA0, changeZoneType 3 — a point (0, 0) is a random type-7 point of the zone, as the
// NPC teleports), and 2000 ms of protection (sub_73CF80(2000, 0)).
void GameClientSession::item_port(const BagItem& b, const ItemDef& d, int zone, int16_t x, int16_t y)
{
    {
        PacketBuffer p(static_cast<Opcode>(0x0097), is_paula());
        p.write<uint64_t>(b.uid);
        p.write<uint16_t>(static_cast<uint16_t>(d.effect));
        std::vector<PacketBuffer> mine, theirs;
        mine.push_back(std::move(p));
        server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    }
    protect(2000);
    LOG_DEBUG("GameClientSession %u: item %u (%s) Effect %d ports to zone %d (%d,%d)", session_id(), d.id,
              d.name.c_str(), d.effect, zone, x, y);
    auto self = shared_from_this();
    schedule_delayed(std::chrono::milliseconds(200), [this, self, zone, x, y]() {
        if (dead_ || leaving_ || state_ != GameClientSessionState::InWorld) return;
        if (x == 0 && y == 0) {
            player_zone_move(zone);
        } else if (leave_field(zone)) {
            begin_field_change(static_cast<int16_t>(zone), x, y, 3);
        }
    });
}

// UseItemHelper::Use (sub_6E1AE0), the item ports. An Option 128 zone keeps its players (S2C 799 [u32 4],
// sub_6028C0: client "TeleportCancelType4").
//   19 Emergency Port: to the return town (+1656, 1..999); already there -> S2C 565 (client
//      "NotExeTeleport").
//   28 brooms (RandomPortParam by the item, db/random_port_db.yml): a zone by its Ratios (sub_6E0010),
//      1..999; the row's Notice in S2C 758 [str] (sub_601C20; client: a system line).
//   custom data (0x8000, C2S 0x0108): 14 [u16 zone] 1..7 ("***INVALID TELEPORT ZONE***" past 7);
//      20 Portable Port AD [u32 index] -> TeleportAdvInfo (db/teleport_adv_db.yml): none -> S2C 567
//      ("ErrorOnUsingTeleportADV"), our own zone -> S2C 566; 39 Friend Finder [u16 x][u16 y][u16 zone] —
//      only in the normal state (not even a sync flag), a zone 1..999 without Option 1024 (else 799 [4]),
//      to that point.
//   Each then ports (item_port) and the item is used up (1). (VERIFIED in the client: sub_6F0960 cases
//   565 / 566 / 567 / 758 / 799.)
int GameClientSession::use_port_item(BagItem& b, const ItemDef& d, bool custom)
{
    auto cancel = [&](uint32_t why) {
        PacketBuffer p(static_cast<Opcode>(0x031F), is_paula());   // S2C 799
        p.write<uint32_t>(why);
        send(p);
        return 0;
    };
    auto empty = [&](uint16_t op) {
        PacketBuffer p(static_cast<Opcode>(op), is_paula());
        send(p);
        return 0;
    };
    if (!custom && d.effect == 19) {
        const int zone = home_;
        if (zone < 1 || zone >= 1000) return 0;
        if (zone == zone_id_) return empty(0x0235);           // S2C 565
        if (zone_no_port(zone_id_)) return cancel(4);
        item_port(b, d, zone, 0, 0);
        return 1;
    }
    if (!custom && d.effect == 28) {
        if (zone_no_port(zone_id_)) return cancel(4);
        const RandomPortDef* row = server_.random_port_db().get(d.id);
        if (!row) return 0;
        const int zone = row->pick();
        if (zone < 1 || zone >= 1000) return 0;
        {
            PacketBuffer n(static_cast<Opcode>(0x02F6), is_paula());   // S2C 758 (sub_601C20)
            const std::string t = to_client_text(row->notice);
            n.write_bytes(t.c_str(), t.size() + 1);
            send(n);
        }
        item_port(b, d, zone, 0, 0);
        return 1;
    }
    if (custom && d.effect == 14) {
        if (zone_no_port(zone_id_)) return cancel(4);
        if (use_data_.left() < 2) {
            LOG_INFO("%s: UseItemHelper:teleport. no custom item use", char_name_);
            return 0;
        }
        const int zone = use_data_.u16();
        if (zone == 0 || zone >= 1000) return 0;
        if (zone > 7) {
            LOG_INFO("%s: ***INVALID TELEPORT ZONE***: eZoneId(%d)", char_name_, zone);
            return 0;
        }
        item_port(b, d, zone, 0, 0);
        return 1;
    }
    if (custom && d.effect == 20) {
        if (zone_no_port(zone_id_)) return cancel(4);
        if (use_data_.left() < 4) {
            LOG_INFO("%s: UseItemHelper:teleportAdv. no custom item use", char_name_);
            return 0;
        }
        const int index = static_cast<int>(use_data_.u32());
        if (!server_.teleport_adv_db().has(index)) return empty(0x0237);   // S2C 567
        const int zone = server_.teleport_adv_db().zone(index);
        if (zone == zone_id_) return empty(0x0236);                          // S2C 566
        item_port(b, d, zone, 0, 0);
        return 1;
    }
    if (custom && d.effect == 39) {
        const bool normal = !dead_ && !talk_npc_ && !su_.active && !pick_.active && !drill_.active && !compound_ &&
                            rest_state_ == 0 && !wh_state_ && !bank_state_ && !sshop_ && !shop_ && !exchange_;
        if (!normal) {
            LOG_INFO("%s: UseItemHelper:TeleportFriend. UserState is not Normal", char_name_);
            return 0;
        }
        if (zone_no_port(zone_id_)) return cancel(4);
        if (use_data_.left() < 6) {
            LOG_INFO("%s: UseItemHelper:TeleportFriend. no custom item use", char_name_);
            return 0;
        }
        const int16_t x = static_cast<int16_t>(use_data_.u16());
        const int16_t y = static_cast<int16_t>(use_data_.u16());
        const int zone  = use_data_.u16();
        if (zone == 0 || zone >= 1000) return 0;
        const MapDef* z = server_.maps_db().get(zone);
        // Hardening (not retail): the zone is the client's (its friend's) — one we have no map of is refused.
        if (!z) {
            LOG_INFO("%s: UseItemHelper:TeleportFriend. unknown zone %d", char_name_, zone);
            return 0;
        }
        if (z->option & 1024) return cancel(4);
        item_port(b, d, zone, x, y);
        return 1;
    }
    return 0;
}

// UseItemHelper::Use cases 24 (Point Back 40: custom data, 12 bytes — the points to take back from each
// stat, 1..40 in all) and 26 (Point Back All) — hItemResetStatus (sub_6E4AA0) + its DB answer (OnOk
// sub_7865B0). No stat may go under its automatic points (its growth x (Lv - 1), the class's P/M/S/C
// growth for AP AC DX / MP MA MD / WT DA LK / HP DP HV); "all" takes each down to them and needs some back.
// The points come back to the unspent pool; the record taking one of the item (sub_6E4460); the stats
// recomputed (sub_7164C0(1)); S2C 629 [33-byte stat block] (sub_600710) or, for all, S2C 634 [block][u32
// points back] (sub_6007B0); the item's S2C 151 [u64 uid][u16 Effect] in a block of its own (sub_5F63F0).
// Failures: S2C 631 / 636 (all). (The retail waits for the DB under the sync flag 0x80000000; it is done
// at once here.) VERIFIED in the client (sub_6B4E00): 0x275 "ResetStatusOk", 0x277 "ResetStatusCancel",
// 0x27A "ResetAllStatusOk" (+ the u32), 0x27C "ResetAllStatusCancel".
int GameClientSession::use_reset_status(BagItem& b, const ItemDef& d, bool all)
{
    auto fail = [&]() {
        PacketBuffer p(static_cast<Opcode>(all ? 0x027C : 0x0277), is_paula());   // S2C 636 / 631
        send(p);
        return 0;
    };
    uint8_t back[12] = {};
    if (!all) {
        if (use_data_.left() < 12) {
            LOG_INFO("%s: UseItemHelper:resetStatus. no custom item use", char_name_);
            return 0;
        }
        int sum = 0;
        for (int i = 0; i < 12; ++i) { back[i] = use_data_.u8(); sum += back[i]; }
        if (sum > 40 || sum <= 0) {
            LOG_INFO("%s: UseItemHelper:resetStatus. fail. SumOfResetValue:%d", char_name_, sum);
            return fail();
        }
    }
    const int gw[12] = { pts_p_, pts_p_, pts_p_, pts_m_, pts_m_, pts_m_, pts_s_, pts_s_, pts_s_, pts_c_, pts_c_, pts_c_ };
    int32_t next[12];
    int bonus = 0;
    for (int i = 0; i < 12; ++i) {
        const int std_pts = gw[i] * (level_ - 1);
        if (accpts_[i] - back[i] < std_pts) {
            LOG_INFO("%s: UseItemHelper:resetStatus. fail. CS_index:%d, PlayerAccPts:%d, ResetValue:%d, StdAccPts:%d",
                     char_name_, i, accpts_[i], back[i], std_pts);
            return fail();
        }
        if (all) { bonus += accpts_[i] - std_pts; next[i] = std_pts; }
        else     { bonus += back[i];              next[i] = accpts_[i] - back[i]; }
    }
    if (all && bonus <= 0) {
        LOG_INFO("%s: hItemResetStatus:resetAllStatus. fail. BonusPoint:%d", char_name_, bonus);
        return fail();
    }
    const uint64_t uid = b.uid;
    const BagRecord rec = bag_decrease(b, 1);                  // sub_6E4460
    if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
    else               server_.item_store().SetAmount(rec.uid, rec.count);
    // OnOk (sub_7865B0)
    bag_apply(rec);
    for (int i = 0; i < 12; ++i) accpts_[i] = next[i];
    attr_pts_ = static_cast<int16_t>(attr_pts_ + bonus);
    recompute_stats(true);
    save_stat_points();
    {
        PacketBuffer p(static_cast<Opcode>(all ? 0x027A : 0x0275), is_paula());   // S2C 634 / 629
        write_stat_block(p);
        if (all) p.write<uint32_t>(static_cast<uint32_t>(bonus));
        send(p);
    }
    {
        PacketBuffer p(static_cast<Opcode>(0x0097), is_paula());                   // S2C 151 (sub_5F63F0)
        p.write<uint64_t>(uid);
        p.write<uint16_t>(static_cast<uint16_t>(d.effect));
        std::vector<PacketBuffer> mine, theirs;
        mine.push_back(std::move(p));
        server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    }
    LOG_INFO("%s: hItemResetStatus::OnOk (%s, %d points back, unspent %d)", char_name_, all ? "all" : "some", bonus,
             attr_pts_);
    return 2;
}

// UseItemHelper::Use case 25 (Build Graph Reset: custom data, 4 bytes — the new Power / Magic / Sense /
// Charm growth, each 1..4, 10 in all, the character's own FType (+1028) at 4) — hItemResetGrowthGraph
// (sub_6E4E10) + its DB answer (OnOk sub_786C50): the growth set (sub_761950), every stat back to its
// automatic points (growth x (Lv - 1)), the unspent pool 34 x (Lv - 1) minus them (sub_73C7A0 checks the sum),
// the stats recomputed, S2C 649 [the 69-byte char info][33-byte stat block] (sub_600970 — VERIFIED: the KR
// client's case 0x289 reads 69 + 33 bytes and says "ResetGrowthGraphOk"; the recovered server wrote a 73-byte
// struct), the item's S2C 151 in a block. Failure: S2C 651 ("ResetGrowthGraphCancel").
int GameClientSession::use_reset_graph(BagItem& b, const ItemDef& d)
{
    if (use_data_.left() < 4) {
        LOG_INFO("%s: UseItemHelper:resetGrowthGraph. no custom item use", char_name_);
        return 0;
    }
    uint8_t g[4] = {};
    int sum = 0;
    bool bad = false;
    for (int j = 0; j < 4; ++j) {
        g[j] = use_data_.u8();
        if (g[j] > 4 || g[j] == 0) { bad = true; break; }
        sum += g[j];
    }
    if (bad || sum != 10 || f_type_ > 3 || g[f_type_] != 4) {
        LOG_INFO("%s: UseItemHelper:resetGrowthGraph. fail. Invalid value : %d, %d, %d, %d", char_name_, g[0], g[1], g[2],
                 g[3]);
        PacketBuffer p(static_cast<Opcode>(0x028B), is_paula());                   // S2C 651
        send(p);
        return 0;
    }
    const uint64_t uid = b.uid;
    const BagRecord rec = bag_decrease(b, 1);
    if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
    else               server_.item_store().SetAmount(rec.uid, rec.count);
    // OnOk (sub_786C50)
    bag_apply(rec);
    pts_p_ = g[0]; pts_m_ = g[1]; pts_s_ = g[2]; pts_c_ = g[3];
    const int lv1 = level_ - 1;
    const int gw[12] = { pts_p_, pts_p_, pts_p_, pts_m_, pts_m_, pts_m_, pts_s_, pts_s_, pts_s_, pts_c_, pts_c_, pts_c_ };
    int total = 0;
    for (int i = 0; i < 12; ++i) { accpts_[i] = gw[i] * lv1; total += accpts_[i]; }
    attr_pts_ = static_cast<int16_t>(34 * lv1 - total);
    recompute_stats(true);
    save_stat_points();
    server_.db().execute("UPDATE characters SET pts_p=%d, pts_m=%d, pts_s=%d, pts_c=%d WHERE uid=%u", pts_p_, pts_m_,
                         pts_s_, pts_c_, char_uid_);
    {
        PacketBuffer p(static_cast<Opcode>(0x0289), is_paula());                   // S2C 649
        uint8_t blob[69] = {};
        build_char_blob(blob);
        p.write_bytes(blob, sizeof(blob));
        write_stat_block(p);
        send(p);
    }
    {
        PacketBuffer p(static_cast<Opcode>(0x0097), is_paula());
        p.write<uint64_t>(uid);
        p.write<uint16_t>(static_cast<uint16_t>(d.effect));
        std::vector<PacketBuffer> mine, theirs;
        mine.push_back(std::move(p));
        server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    }
    LOG_INFO("%s: UseItemHelper:resetGrowthGraph. succeed. growth %d/%d/%d/%d, BonusPts:%d", char_name_, pts_p_, pts_m_,
             pts_s_, pts_c_, attr_pts_);
    return 2;
}

// UseItemHelper::Use case 15 (hair dye: custom data [u8 hair], under 21 — ePType_Max) — hItemHairColor
// (sub_6E42B0) + its DB answer (OnOk sub_7855F0): the record taking one, the DB (3656: the hair of the shown
// tier, job_state +4680 1 / 2 / 3 -> hair / hair_2 / hair_3), then Player +440 = the hair, S2C 337 [u8 hair]
// to us and S2C 338 [u16 id][u8 hair] to the area (sub_5F5EC0), the item's S2C 151 in a block. VERIFIED in the
// client: sub_6F0960 case 337 -> sub_6EC560 (the avatar's hair, "Alert_ChangedHairColor"), case 338 ->
// sub_6F0260 (another actor's).
// UseItemHelper::Use case 21 (the custom data's [u16], below 9 = eChrType_Boxer, not used further) -> hItemChangeChrType
// (sub_6E4670, Befana's elixir): a character before its 2nd job (EXE class < 9 — our 1st-tier jobs 1..8; nothing said
// otherwise), its worn items fitting the free bag places (sub_760F80) and 2 free places, of type 1..8 -> the paired
// type (1<->2, 3<->4, 5<->6, 7<->8: the other sex of the same animal), job = that type, hair 0 (Master 3666 ->
// uspm_char_attr_update_transgender: no 2nd / 3rd job, |old - new| <= 1, type / job / transgender_date, hair, the elixir
// -1 or gone) -> OnOk (sub_785E50): the elixir's record; SetChangeCharType (sub_761620): the type and job, the worn items
// of Types 5 / 17..27 / 34 off (sub_760FF0), the new type's ears (6899 + type) and tail (6949 + type) into the bag
// (hItemGive sub_78BA40 -> S2C 586 [u16 n]{record}); S2C 584 [u16 us][u16 type][u8 job][u8 0] and 582 [us][the worn
// look] to the area (VERIFIED: client case 584 -> the actor's "change" effect + etc_levelup; 582); the party's view
// (SYSCMD 5535); the use block S2C 151 [u64 uid][u16 21]. A refusal: S2C 585 (client: WordStrings ChangeJob_Cancel).
int GameClientSession::use_char_type_change(BagItem& b, const ItemDef& d)
{
    auto refuse = [&]() {                                      // _fputchar_91: S2C 585
        PacketBuffer p(static_cast<Opcode>(585), is_paula());
        send(p);
        return 0;
    };
    if (use_data_.left() < 2) {
        LOG_INFO("%s: UseItemHelper:chageChar. no custom item use", char_name_);
        return refuse();
    }
    if (use_data_.u16() >= 9) {
        LOG_INFO("%s: UseItemHelper:chageChar. Chartype >= eChrType_Boxer", char_name_);
        return refuse();
    }
    if (job_ >= 9) return 0;                                   // (sub_6E4670: class < 9, else nothing)
    int worn = 0;
    for (uint64_t u : equip_) if (u) ++worn;
    const int free_places = 300 - static_cast<int>(bag_.size());   // sub_6E4290
    if (worn > free_places || free_places < 2) return refuse();
    static const int16_t kPair[9] = { 0, 2, 1, 4, 3, 6, 5, 8, 7 };
    if (base_type_ < 1 || base_type_ > 8) {
        LOG_ERROR("%s: hItemChangeChrType:Send. Char >= eChrType_Raccoon", char_name_);
        return refuse();
    }
    const int16_t nt = kPair[base_type_];
    // uspm_char_attr_update_transgender
    if (job2_ || job3_) {
        LOG_ERROR("uspm_char_attr_update_transgender(101) Not found char or already job chaneged %u, %d", char_uid_, nt);
        return refuse();                                       // OnCancel (sub_785F80)
    }
    if (server_.db().execute("UPDATE characters SET type=%d, job=%d, transgender_date=NOW(), hair=0 WHERE uid=%u", nt,
                             nt, char_uid_) == 0) {
        LOG_ERROR("uspm_char_attr_update_transgender(103) DB Error(Update char_attr) %u, %d", char_uid_, nt);
        return refuse();
    }
    const uint64_t uid = b.uid;
    const BagRecord rec = bag_decrease(b, 1);                  // sub_6E4460
    if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
    else               server_.item_store().SetAmount(rec.uid, rec.count);
    // OnOk
    bag_apply(rec);
    base_type_ = nt;
    job1_      = static_cast<uint8_t>(nt);
    hair_      = 0;
    apply_job_state();                                         // the shown model and the class
    {                                                          // sub_760FF0(1)
        std::vector<uint64_t> release;
        for (int i = 0; i < 23; ++i) {
            const BagItem* e = equip_[i] ? bag_find_uid(equip_[i]) : nullptr;
            const ItemDef* ed = e ? server_.item_db().get(e->item_id) : nullptr;
            if (ed && (ed->type == 5 || (ed->type >= 17 && ed->type <= 27) || ed->type == 34)) release.push_back(equip_[i]);
        }
        if (!release.empty()) equip_n_release(-1, release, 0);
    }
    {                                                          // hItemGive: the new type's ears and tail
        std::vector<QuestItem> give;
        for (uint32_t id : { 6899u + static_cast<uint32_t>(nt), 6949u + static_cast<uint32_t>(nt) })
            if (server_.item_db().get(id)) give.push_back(QuestItem{ id, 1 });
        const std::vector<BagRecord> recs = quest_bag_records({}, give);
        if (!recs.empty()) {
            PacketBuffer p(static_cast<Opcode>(0x024A), is_paula());   // S2C 586 (sub_5FEED0)
            p.write<uint16_t>(static_cast<uint16_t>(recs.size()));
            for (const BagRecord& r : recs) write_bag_record(p, r);
            send(p);
        }
    }
    {
        PacketBuffer p(static_cast<Opcode>(0x0248), is_paula());       // S2C 584 (sub_5FEE00)
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<uint16_t>(static_cast<uint16_t>(type_));
        p.write<uint8_t>(static_cast<uint8_t>(job_));
        p.write<uint8_t>(0);
        server_.broadcast_zone(zone_id_, p, 0);
    }
    {
        PacketBuffer p(static_cast<Opcode>(0x0246), is_paula());       // S2C 582 (sub_5FEC70)
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        write_equip_appearance(p);
        server_.broadcast_zone(zone_id_, p, 0);
    }
    server_.party().update(char_uid_, party_info());               // SYSCMD 5535 (the party / friends' view)
    {
        PacketBuffer p(static_cast<Opcode>(0x0097), is_paula());       // S2C 151 (sub_5F63F0)
        p.write<uint64_t>(uid);
        p.write<uint16_t>(static_cast<uint16_t>(d.effect));
        std::vector<PacketBuffer> mine, theirs;
        mine.push_back(std::move(p));
        server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    }
    LOG_INFO("%s : hItemChangeChrType::OnOk (type %d, job %d)", char_name_, type_, job_);
    return 2;
}

int GameClientSession::use_hair_dye(BagItem& b, const ItemDef& d)
{
    if (use_data_.left() < 1) {
        LOG_INFO("%s: UseItemHelper:chagehair. no custom item use", char_name_);
        return 0;
    }
    const uint8_t hair = use_data_.u8();
    if (hair >= 21) {
        LOG_INFO("%s: UseItemHelper:chagehair. hair >= ePType_Max", char_name_);
        return 0;
    }
    const char* col = (job_state_ == 2) ? "hair_2" : (job_state_ == 3) ? "hair_3" : (job_state_ == 1) ? "hair" : nullptr;
    if (!col) {                                                // (the retail sends no DB request then)
        LOG_ERROR("%s: hItemHairColor: job_state %d", char_name_, job_state_);
        return 0;
    }
    const uint64_t uid = b.uid;
    const BagRecord rec = bag_decrease(b, 1);                  // sub_6E4460
    if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
    else               server_.item_store().SetAmount(rec.uid, rec.count);
    server_.db().execute("UPDATE characters SET %s=%d WHERE uid=%u", col, hair, char_uid_);
    // OnOk (sub_7855F0)
    bag_apply(rec);
    if (job_state_ == 2) hair2_ = hair;
    else if (job_state_ == 3) hair3_ = hair;
    else hair_ = hair;
    {
        PacketBuffer p(static_cast<Opcode>(0x0151), is_paula());                   // S2C 337
        p.write<uint8_t>(hair);
        send(p);
    }
    {
        PacketBuffer p(static_cast<Opcode>(0x0152), is_paula());                   // S2C 338
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<uint8_t>(hair);
        server_.broadcast_zone(zone_id_, p, session_id());
    }
    {
        PacketBuffer p(static_cast<Opcode>(0x0097), is_paula());                   // S2C 151
        p.write<uint64_t>(uid);
        p.write<uint16_t>(static_cast<uint16_t>(d.effect));
        std::vector<PacketBuffer> mine, theirs;
        mine.push_back(std::move(p));
        server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    }
    LOG_INFO("%s : hItemHairColor::OnOk (hair %d, tier %d)", char_name_, hair, job_state_);
    return 2;
}

// UseItemHelper::Use case 23 ("Master's Authority": custom data [u16 skill]) — hItemResetSkill (sub_6E48F0) +
// its DB answer (OnOk sub_785FF0): not a skill id >= 5000 ("SPECIAL"), not a pre-skill of a learned skill
// (sub_892480: PreSkill_1..3 of their cards), a learned one — else S2C 620 ("ResetSkillCancel"). The TM
// points back (sub_88F230): the card's AffectionPoint + (level - RegisterLevel), a mastered (11) one counting
// MasterTMPoint for its last level instead of 1; the record taking one, the DB (4514), then the skill is
// forgotten (sub_892360) and its cool time (sub_88F7C0), TM points += them, S2C 618 [u16 skill][u16 points]
// (sub_600570; VERIFIED: client case 0x26A forgets it, adds the points, "ResetSkillOk"), the item's S2C 151.
int GameClientSession::use_reset_skill(BagItem& b, const ItemDef& d)
{
    auto fail = [&]() {
        PacketBuffer p(static_cast<Opcode>(0x026C), is_paula());   // S2C 620
        send(p);
        return 0;
    };
    if (use_data_.left() < 2) {
        LOG_INFO("%s: UseItemHelper:resetSkill. no custom item use", char_name_);
        return 0;
    }
    const int skill = use_data_.u16();
    if (skill >= 5000) {
        LOG_INFO("%s: UseItemHelper:resetSkill. try to reset SPECIAL skill. skill_ID:%d", char_name_, skill);
        return fail();
    }
    for (const LearnedSkill& ls : skills_) {                    // sub_892480
        const SkillDef* sd = server_.skill_db().get(ls.id);
        const SkillCardDef* c = sd ? server_.skill_card_db().get(static_cast<uint32_t>(sd->card)) : nullptr;
        if (!c) continue;
        for (int i = 0; i < 3; ++i) {
            if (c->pre_skill[i] == skill) {
                LOG_INFO("%s: UseItemHelper:resetSkill. try to reset PREV skill. skill_ID:%d", char_name_, skill);
                return fail();
            }
        }
    }
    const LearnedSkill* ls = find_skill(skill);
    if (!ls) {
        LOG_INFO("%s: UseItemHelper:resetSkill. try to reset HaveNo skill. skill_ID:%d", char_name_, skill);
        return fail();
    }
    const SkillDef* sd = server_.skill_db().get(skill);
    const SkillCardDef* card = sd ? server_.skill_card_db().get(static_cast<uint32_t>(sd->card)) : nullptr;
    int lv_gain = ls->level - (card ? card->register_level : 0);   // sub_88F230
    int master = 0;
    if (ls->level == 11) { --lv_gain; master = sd ? sd->master_tm : 0; }
    const int back = master + lv_gain + (card ? card->affection_point : 0);
    const int level = ls->level;
    const uint64_t uid = b.uid;
    const BagRecord rec = bag_decrease(b, 1);                  // sub_6E4460
    if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
    else               server_.item_store().SetAmount(rec.uid, rec.count);
    server_.db().execute("DELETE FROM char_skills WHERE char_uid=%u AND skill_id=%d", char_uid_, skill);
    // OnOk (sub_785FF0)
    bag_apply(rec);
    skills_.erase(std::remove_if(skills_.begin(), skills_.end(), [&](const LearnedSkill& s) { return s.id == skill; }),
                  skills_.end());
    skill_cd_.erase(static_cast<uint16_t>(skill));
    tm_points_ += back;
    save_stat_points();
    recompute_stats(true);                                     // (a passive's adder goes with it)
    {
        PacketBuffer p(static_cast<Opcode>(0x026A), is_paula());                   // S2C 618
        p.write<uint16_t>(static_cast<uint16_t>(skill));
        p.write<uint16_t>(static_cast<uint16_t>(back));
        send(p);
    }
    {
        PacketBuffer p(static_cast<Opcode>(0x0097), is_paula());                   // S2C 151
        p.write<uint64_t>(uid);
        p.write<uint16_t>(static_cast<uint16_t>(d.effect));
        std::vector<PacketBuffer> mine, theirs;
        mine.push_back(std::move(p));
        server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    }
    LOG_INFO("%s: UseItemHelper:resetSkill. Succeed. skill_ID:%d (lv %d), RecoveryTMP:%d", char_name_, skill, level, back);
    return 2;
}

// ── The disguise ───────────────────────────────────────────────────────────────────────────────────────
//  UseItemHelper::Use SubType 71 ('G'): the item's TransScrollInfo model (sub_63D7C0; none -> S2C 1024)
//  becomes Player +6208 (sub_769530: only a model 1..18033; +6204 = it is not our type — even when the
//  use then fails, retail); disguised and in the normal StateType (+1060 bit 1, sub_718CD0(1)) -> StateType
//  2 | 16 (sub_718CF0(16)) and S2C 1023 [u16 id][u32 model] to the area (sub_769580 -> sub_607270), 1 (the
//  dispatcher's S2C 0x0059: the helper sent no effect); else S2C 1024. Others see the model as the appear's
//  "compact appearance" (sub_73AB30). VERIFIED in the client (sub_6F0960): 1023 polymorphs the player and
//  plays "change"; 1024 says TransScrollStrings "TransScroll_Cancel"; 1026 [u16 id][u8 effect] turns it back
//  (the effect "change" when the u8 is set); 582 refreshes its worn items.
int GameClientSession::use_disguise_kit(BagItem& b, const ItemDef& d)
{
    auto fail = [&]() {
        PacketBuffer p(static_cast<Opcode>(0x0400), is_paula());   // S2C 1024 (_fputchar_129)
        send(p);
        return 0;
    };
    const int model = server_.trans_scroll_db().model(d.id);
    if (!model) return fail();
    set_disguise_model(model);
    if (!disguised_ || !(state_type_ & 1)) return fail();
    state_type_ = 2 | 16;                                      // sub_718CF0(16)
    {
        PacketBuffer p(static_cast<Opcode>(0x03FF), is_paula());   // S2C 1023
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<uint32_t>(static_cast<uint32_t>(shown_model_));
        server_.broadcast_zone(zone_id_, p, 0);
    }
    {
        PacketBuffer u(static_cast<Opcode>(0x0059), is_paula());   // the dispatcher (sub_6DFA70 -> sub_6E1590)
        u.write<uint64_t>(b.uid);
        send(u);
    }
    LOG_DEBUG("GameClientSession %u: disguised as model %d by %u (%s)", session_id(), shown_model_, d.id, d.name.c_str());
    return 1;
}

// sub_769530: the model to show — taken only in 1..18033; disguised when it is not our own type.
void GameClientSession::set_disguise_model(int model)
{
    if (model >= 1 && model < 18034) {
        shown_model_ = model;
        disguised_   = (model != type_);
    }
}

// sub_7695B0's body (and sub_76AA00's): back to our own model, StateType 1, S2C 1026 [u16 id][u8 effect]
// (sub_607320) and S2C 582 [u16 id][worn items] (sub_5FEC70) to the area.
void GameClientSession::disguise_end(uint8_t effect)
{
    set_disguise_model(type_);
    state_type_ = 1;                                           // sub_718CF0(1)
    {
        PacketBuffer p(static_cast<Opcode>(0x0402), is_paula());   // S2C 1026
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<uint8_t>(effect);
        server_.broadcast_zone(zone_id_, p, 0);
    }
    {
        PacketBuffer p(static_cast<Opcode>(0x0246), is_paula());   // S2C 582
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        write_equip_appearance(p);
        server_.broadcast_zone(zone_id_, p, 0);
    }
    LOG_DEBUG("GameClientSession %u: disguise ends", session_id());
}

void cGameServer::disguise_put(uint32_t char_uid, int32_t model)
{
    std::lock_guard<std::mutex> lock(disguise_carry_mutex_);
    disguise_carry_[char_uid] = { model, std::chrono::steady_clock::now() };
}

int32_t cGameServer::disguise_take(uint32_t char_uid)
{
    std::lock_guard<std::mutex> lock(disguise_carry_mutex_);
    auto it = disguise_carry_.find(char_uid);
    if (it == disguise_carry_.end()) return 0;
    const auto v = it->second;
    disguise_carry_.erase(it);
    if (std::chrono::steady_clock::now() - v.second > std::chrono::seconds(60)) return 0;
    return v.first;
}

// C2S 0x0401 (Player case 1025, sub_7695B0): disguised, StateType 16 and a model not ours -> disguise_end(0);
// else S2C 1027 (_fputchar_130) — which the KR client has no case for: not sent.
void GameClientSession::handle_disguise_cancel(PacketBuffer& /*pkt*/)
{
    if (!disguised_ || !(state_type_ & 16) || shown_model_ == type_) {
        LOG_DEBUG("GameClientSession %u: C2S 0x0401 disguise cancel refused (S2C 1027 has no client handler)",
                  session_id());
        return;
    }
    disguise_end(0);
}

// UseItemHelper::Use (sub_6E1AE0) case 27 — hItemAddWarehouseSlot (sub_6E44B0), the Store-More Permit:
// 8 more warehouse slots, at most 800 (else "over max slot cnt", S2C 743 and 0). The record taking one
// of the item (sub_6E4460), the DB (3681 -> uspm_char_state_update_wh_slots: wh_slots + 8, the item
// updated (flag 2) or deleted (0)) -> OnOk (sub_7857E0): the record applied, the warehouse grows
// (sub_6F2BA0: a new tab is read on the next opening), S2C 151 [u64 uid][u16 27] in a command block of
// its own (sub_5F63F0; the client takes one off), S2C 742 [u8 8] (sub_6018B0; client sub_BC3020: the
// slots). A DB failure: OnCancel (sub_785DE0) S2C 90 and S2C 743 ("WHMSG_AppendSlotCancel"). The helper
// answers 2: UseItem takes nothing more. (VERIFIED in the client: sub_6F0960 cases 742 / 743.)
int GameClientSession::use_warehouse_slot_item(BagItem& b, const ItemDef& d)
{
    constexpr int kIncrease = 8;
    auto cancel = [&]() {
        PacketBuffer c(static_cast<Opcode>(0x02E7), is_paula());       // S2C 743 (__fputchar_110)
        send(c);
    };
    if (kIncrease + wh_slots_ > 800) {
        LOG_ERROR("[warehouse] %s : hItemAddWarehouseSlot::over max slot cnt.(curSlotCnt:%d, increaseSlotCnt:%d)",
                  char_name_, wh_slots_, kIncrease);
        cancel();
        return 0;
    }
    const BagRecord rec = bag_decrease(b, 1);
    const uint64_t uid = b.uid;
    // uspm_char_state_update_wh_slots (statement by statement)
    if (server_.db().query("SELECT uid FROM characters WHERE uid=%u", char_uid_).empty()) {
        LOG_ERROR("uspm_char_state_update_wh_slots(101) %u, %d", char_uid_, kIncrease);
        PacketBuffer x(static_cast<Opcode>(0x005A), is_paula());       // S2C 90 (sub_5F6730)
        send(x);
        cancel();
        return 2;
    }
    server_.db().execute("UPDATE characters SET wh_slots=wh_slots+%d WHERE uid=%u", kIncrease, char_uid_);
    if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
    else               server_.item_store().SetAmount(rec.uid, rec.count);
    // OnOk
    bag_apply(rec);
    wh_build(kIncrease);
    {
        PacketBuffer p(static_cast<Opcode>(0x0097), is_paula());       // S2C 151
        p.write<uint64_t>(uid);
        p.write<uint16_t>(static_cast<uint16_t>(d.effect));
        std::vector<PacketBuffer> mine, theirs;
        mine.push_back(std::move(p));
        server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    }
    PacketBuffer s(static_cast<Opcode>(0x02E6), is_paula());           // S2C 742
    s.write<uint8_t>(static_cast<uint8_t>(kIncrease));
    send(s);
    LOG_INFO("[warehouse] %s : hItemAddWarehouseSlot::OnOk (item %u, slots %d, %zu tabs)", char_name_, d.id, wh_slots_,
             wh_tabs_.size());
    return 2;
}

// sub_6E15C0 -> sub_5F64F0, inside the command block of the use (sub_41C6F0 .. sub_41C700): S2C 0x0097
// [u64 uid][effect] to the user (client: the block's opener -> sub_6D2190, the user's item effect,
// then the rest of the block), S2C 0x005B [u16 user][u32 item id][effect] to the area (others: the
// opener of their block -> sub_6D1FA0, the effect on the user; the user's own 0x005B is ignored),
// then `mine_after` (the user's HP / MP / money) — one block id, closed for each recipient.
void GameClientSession::item_effect_block(const BagItem& b, const ItemDef& d, const std::vector<uint8_t>& effect,
                                          std::vector<PacketBuffer> mine_after)
{
    const uint16_t self = static_cast<uint16_t>(object_id_);
    PacketBuffer own(static_cast<Opcode>(0x0097), is_paula());
    own.write<uint64_t>(b.uid);
    own.write_bytes(effect.data(), effect.size());
    PacketBuffer area(static_cast<Opcode>(0x005B), is_paula());
    area.write<uint16_t>(self);
    area.write<uint32_t>(d.id);
    area.write_bytes(effect.data(), effect.size());

    std::vector<PacketBuffer> mine, theirs;
    mine.push_back(std::move(own));
    mine.push_back(area);
    for (PacketBuffer& p : mine_after) mine.push_back(std::move(p));
    theirs.push_back(std::move(area));
    server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
}

namespace {
// sub_4FFFA0: the name the GameServer takes — not empty; not starting with '.', '\'' or '/'; no byte <= 0x20, '`', ','
// or '\''; a double-byte pair (a lead >= 0x80 with a byte after it) without 0xFF, not 0xC7.. with a trail 0x41..0xA0,
// not 0xC6 with 0x53..0xA0, not a lead 0xC9 / 0xFE, not 0xA4D4; a last '.' refused; 4..16 bytes.
bool game_name_ok(const std::string& s)
{
    if (s.empty()) return false;
    const unsigned char first = static_cast<unsigned char>(s[0]);
    if (first == '.' || first == '\'' || first == '/') return false;
    size_t i = 0;
    for (; i < s.size(); ++i) {
        const unsigned char v3 = static_cast<unsigned char>(s[i]);
        if (v3 <= 0x20) return false;
        if (v3 == '`' || v3 == ',' || v3 == '\'') return false;
        if (i + 1 < s.size()) {
            const unsigned char v2 = static_cast<unsigned char>(s[i + 1]);
            if (v3 >= 0x80) {
                if (v3 == 0xFF || v2 == 0xFF) return false;
                if (v3 >= 0xC7 && v2 >= 0x41 && v2 <= 0xA0) return false;
                if (v3 == 0xC6 && v2 >= 0x53 && v2 <= 0xA0) return false;
                if (v3 == 0xC9 || v3 == 0xFE) return false;
                if (((static_cast<unsigned>(v3) << 8) | v2) == 0xA4D4) return false;
                ++i;
            }
        } else if (v3 == '.') {
            return false;
        }
    }
    return i >= 4 && i <= 16;
}
} // namespace

// UseItemHelper::Use case 0x1F (Effect 31, items 19647 / 19711 / 19783; hItemChangeCharName sub_6E4F60 and the Master's
// OnCCmdChangeCharName sub_409F80 -> uspm_insert_char_names_changed, its answers OnOk sub_7892C0 / OnCancel sub_789640):
//   the custom data 1..17 bytes, else 0; its name (sub_43AD60) refused by sub_4FFFA0 -> S2C 789 [u32 1019] (sub_602600)
//   and 0; else (a request already running, +1636 0x80000000 -> nothing) one of the item held back and CCMD 3690 to the
//   Master; the use answers 2. The Master: 1023 the name taken / 1019 not allowed / 101 / 102 the writes -> 3692 [code]
//   -> OnCancel: S2C 789 [u32 code] (client: the rename window's error, sub_BDEAF0); 0 -> 3691 (and 5542 [uid][old]
//   [new] to the SystemServers: only the couple server takes it) -> OnOk: the item's record applied, S2C 788 [record]
//   (client sub_6F0960 case 788: the bag, the window's success, sub_BDE7F0). The Player keeps its old name (+60)
//   until it logs in again (retail: nothing renames the live objects). Here the Master's step runs at once.
int GameClientSession::use_rename_item(BagItem& b, const ItemDef& d)
{
    if (use_data_.left() == 0 || use_data_.left() > 17) return 0;
    std::string name;
    if (!use_data_.str(name)) return 0;
    if (!game_name_ok(name)) {
        PacketBuffer p(static_cast<Opcode>(0x0315), is_paula());          // S2C 789
        p.write<uint32_t>(1019);
        send(p);
        return 0;
    }
    std::string old;
    const int err = server_.char_store().RenameCharacter(char_uid_, name, d.id, old);
    if (err) {                                                            // OnCancel (sub_789640)
        PacketBuffer p(static_cast<Opcode>(0x0315), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(err));
        send(p);
        LOG_INFO("%s: rename to '%s' refused (%d)", char_name_, name.c_str(), err);
        return 2;
    }
    const BagRecord rec = bag_decrease(b, 1);                             // OnOk (sub_7892C0)
    if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
    else               server_.item_store().SetAmount(rec.uid, rec.count);
    bag_apply(rec);
    PacketBuffer p(static_cast<Opcode>(0x0314), is_paula());              // S2C 788 (sub_602580)
    write_bag_record(p, rec);
    send(p);
    LOG_INFO("CCMD_CHANGE_CHAR_NAME_ACK_OK (%u): %s => %s", char_uid_, old.c_str(), name.c_str());
    return 2;
}
