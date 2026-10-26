#include "game_server.hpp"
#include "game_session.hpp"
#include "combat_util.hpp"
#include "item_wire.hpp"
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  Item refine, repair, mature (growth) compound and its reset — 1:1 with the retail GameServer
//  (GameServerRelease) and the KR 2014-01 client.
//
//  The NPC scripts (ExecuteNpcScript sub_7E85A0) put the player into the UI states:
//    func 71 Item_Refine (sub_7E5380): state 30 + S2C 710 (empty — the ItemRefineUI opens);
//    func 72 Item_Growth (sub_7E53A0): state 31 + S2C 711 (the mature-compound window);
//    func 73 Reset_Mix   (sub_7E53C0): state 32 + S2C 739 (the reset window).
//  The states swallow C2S 0 / 0x18 (move) / 0x40 (sit) / 0x51 (equip) / 0x205 (the common slot) and
//  end with their close command (702 / 707 / 737 -> state 0), a zone change or death.
//
//  REFINE — C2S 696 [u64 item][u64 material][u64 support][u32 support row Id] (client c2s_696;
//  hItemRefine::Send sub_782800 + OnOk sub_782F70; the retail answers through the DB, msgs 4124/
//  4125/4126 + events 30161/30162 — ours is synchronous):
//    the item: in the bag, not equipped (sub_6DA7F0), ItemParam RefineType 1..7 and refine level
//      (byte +49 = rare[1]) under 13 (sub_782F20), not refine-failed (sub_6DE920: byte +50);
//    the refined option: dword_9C3238[RefineType] = 10000 AP / 10010 DP / 10004 MA / 10005 MD /
//      10012 GunAp / 10007 DA / 10011 HV — the item must already have the type-10 option of that id
//      (sub_6E82B0); its value is the base;
//    the cost: Options 0x40000 (the ItemParam flags, or a rare option 13 override) -> 100 x next
//      level x MinLevel, else next level x Value; minus the ItemRefineCostDiscount ratio (0);
//    the material: ItemRefineTable[RefineType-1], the row of the item's MinLevel band, its
//      ItemIdLv/ItemCntLv of the next level — the given uid must be that item with enough of it;
//    the support (optional, row Id != 0): RefineSupportItemLevel — the given uid must be the row's
//      ItemId; adds Prob % and consumes ItemCount (an invalid one is ignored, retail);
//    the rate: RefineLevelTable[RefineIndex].RatioLv[next] x (1 + the fortune card's refine stat x
//      RefineSeed/100 / 100) + Prob, at most 100 (the card system does not exist here: no bonus);
//      rand(10000) under rate x 100 = success;
//    success: the type-11 option of the stat is inserted (level 0) or set (sub_6E76F0 / sub_6E7A90)
//      to AccumulationLv[next] x base / 100, the level byte +1 (uspm_item_update_refine_success);
//    failure: byte +50 = 1 — the item refuses to equip until repaired (uspm_item_update_refine_state
//      1); the money and the materials are spent either way.
//    S2C 697 [u8 success][record material][record support (type 0 = none)][u64 item][u16 option id]
//    [u16 value][u32 money]; a refused request = S2C 698 (empty). VERIFIED in the client (sub_6F0960
//    case 697: reads exactly that, adds/updates the type-11 option, ++byte 49 / byte 50 = 1).
//
//  REPAIR — C2S 699 [u64 item][u64 material] (hItemRepair::Send sub_783850 + OnOk sub_783B10):
//    the item: refineable (sub_782F20) and refine-FAILED (sub_6DE920); the material: any SubType-63
//    item (Repair Powder 19629 / 64799), max(1, refine level) of it; no money. Byte +50 = 0
//    (uspm_item_update_refine_state 0). S2C 700 [u64 item][record]; refused = S2C 701.
//
//  MATURE COMPOUND — C2S 704 [u64 item][u64 material][u64 aid1][u64 aid2] (client c2s_704;
//  hItemGrowthCompound::Send sub_784030 + OnOk sub_784B00; DB msgs 4130/4131/4132, events 30165/6):
//    the aids (Maturing_Compound_Help): two must not boost the same field ("aid item is
//      duplicated"); their ApplyRatio / MinAbility / MaxAbility sum (an aid without a row is
//      consumed for nothing — retail);
//    the item: in the bag, not equipped, an Equip_Property row, a free slot among 0..9 under the
//      item's slot count (byte +51 = rare[3], sub_6E8870), money >= the cost (5 x Value minus the
//      Maturing_Compound_CostDiscount ratio (0)), and not refine-failed unless Options 0x40000;
//    the material (Maturing_Compound): its EnableProperty among the equip's 10, ItemNum of it in
//      the bag, the item's MinLevel >= EquipLv;
//    the value: extraMin = MinAbility x the aids' MinAbility sum (at least +-1 towards MinAbility's
//      sign when aided), extraMax likewise; the lucky chance = (Equip_Property ApplyRatio + the
//      aids' ApplyRatio) x (DA/(DA + MinLevel + 10) + DA/100) x the Compound Mastery (3014) ESA
//      CompoundRatio, x 100; rand(100) under it -> the Import curve's roll between MinAbility +
//      extraMin and MaxAbility + extraMax (sub_6184A0: min' + (max'-min') x curve[rand(10000)] /
//      100), else exactly MinAbility + extraMin;
//    the option {type = the slot, id = EnableProperty, value, extra = the material's item id} is
//      added (uspm_item_update_remix_success) and the material + one of each aid consumed.
//    S2C 705 [record material][u64 item][u8 slot][u16 option id][i16 value][u32 material item]
//    [9 zero bytes — the 2014 client reads a second property the 2012 server never sends]
//    [u32 money][record aid1][record aid2]; refused = S2C 706. VERIFIED in the client (case 705).
//
//  RESET — C2S 734 [u64 item][u64 material][u32 slot] (client c2s_734; hItemGrowthCompoundReset
//  sub_787780 / sub_787B20; DB msgs 4134/4135/4136, events 30184/5):
//    the item: an Equip_Property row ("can't mixable"), at least one slot option (sub_6E8A40), not
//    equipped; the material: item 19641 (Nate's Bottle), 1 of it; the slot's option must exist. The
//    option goes (uspm_item_option_delete_remix). S2C 735 [u64 item][u32 slot][u32 0 — the second
//    slot the 2014 client reads][record]; refused = S2C 736. VERIFIED in the client (case 735).
//
//  Not modelled: the pet's reaction to a refine (sub_738F50(pet, 19/18) — pet speech is not done),
//  the game-log writers (sub_783280 / sub_784D20 / sub_787D30) and the statistics counters
//  (sub_7B3B60(23/24, 1)).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// dword_9C3238: ItemParam RefineType -> the refined option id.
constexpr int16_t kRefineProp[8] = { 0, 10000, 10010, 10004, 10005, 10012, 10007, 10011 };

constexpr uint32_t kResetMaterial  = 19641;   // Nate's Bottle (hardcoded, sub_787780)
constexpr int      kRepairSubType  = 63;      // Repair Powder (sub_783AF0: ItemParam SubType 63)

} // namespace

// The item's Options, or a rare item's type-13 option override (sub_6ECC40 / sub_6ECDF0).
static uint32_t item_flags(uint8_t kind, const std::vector<ItemOption>& opts, const ItemDef& d)
{
    uint32_t flags = d.options;
    if (kind == 2)
        for (const ItemOption& o : opts)
            if (o.type == 13) { flags = static_cast<uint32_t>(o.extra); break; }
    return flags;
}

// sub_6E82B0-like: the item's option of that type and id, or null.
const ItemOption* GameClientSession::item_option(const BagItem& b, int type, int16_t id) const
{
    for (const ItemOption& o : b.opts)
        if (o.type == static_cast<uint32_t>(type) && o.id == id) return &o;
    return nullptr;
}

// ── The NPC scripts ──────────────────────────────────────────────────────────

void GameClientSession::refine_ui_enter(int state)
{
    rest_state_ = 0;                                     // (the UI states replace the others)
    shop_       = 0;
    exchange_   = false;
    drill_      = Drill{};
    wh_leave_state();
    skill_shop_leave();
    compound_leave();
    npc_end_talk();                                      // ChangeState leaves the talk state 9
    refine_ui_  = state;
}

void GameClientSession::refine_ui_leave()
{
    refine_ui_ = 0;
}

// Item_Refine (func 71, sub_7E5380): state 30, S2C 710 (empty).
int GameClientSession::npc_refine_start()
{
    refine_ui_enter(30);
    PacketBuffer p(static_cast<Opcode>(0x02C6), is_paula());
    send(p);
    LOG_DEBUG("GameClientSession %u: Item_Refine — state 30", session_id());
    return 0;
}

// Item_Growth (func 72, sub_7E53A0): S2C 711, state 31.
int GameClientSession::npc_growth_start()
{
    refine_ui_enter(31);
    PacketBuffer p(static_cast<Opcode>(0x02C7), is_paula());
    send(p);
    LOG_DEBUG("GameClientSession %u: Item_Growth — state 31", session_id());
    return 0;
}

// Reset_Mix (func 73, sub_7E53C0): S2C 739, state 32.
int GameClientSession::npc_resetmix_start()
{
    refine_ui_enter(32);
    PacketBuffer p(static_cast<Opcode>(0x02E3), is_paula());
    send(p);
    LOG_DEBUG("GameClientSession %u: Reset_Mix — state 32", session_id());
    return 0;
}

// C2S 702 / 707 / 737: the window closed — state 0 (no answer).
void GameClientSession::handle_refine_close(uint16_t op)
{
    const int state = (op == 0x02BE) ? 30 : (op == 0x02C3) ? 31 : 32;
    if (refine_ui_ == state) refine_ui_leave();
}

// ── Refine (C2S 696, state 30) ───────────────────────────────────────────────

void GameClientSession::handle_refine(PacketBuffer& pkt)
{
    auto refuse = [&]() {                                        // __fputchar_105: S2C 698
        PacketBuffer p(static_cast<Opcode>(0x02BA), is_paula());
        send(p);
    };
    try {
        if (refine_ui_ != 30) return;
        const uint64_t uid     = pkt.read<uint64_t>();
        const uint64_t mat_uid = pkt.read<uint64_t>();
        const uint64_t sup_uid = pkt.read<uint64_t>();
        const uint32_t sup_id  = pkt.read<uint32_t>();

        BagItem* b = bag_find_uid(uid);
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d) {
            LOG_INFO("%s : hItemRefine::Send - not found (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(uid));
            refuse(); return;
        }
        // sub_782F20 + the Send checks: refineable, not equipped, not failed, the money.
        const int next = b->rare[1] + 1;
        const int16_t prop = (d->refine_type > 0 && d->refine_type < 8) ? kRefineProp[d->refine_type] : 0;
        const ItemOption* base = prop ? item_option(*b, 10, prop) : nullptr;
        if (prop && !base) {
            LOG_INFO("%s : hItemRefine::Send - not exist property (itemUid:%llu, prop:%d)", char_name_,
                     static_cast<unsigned long long>(uid), prop);
            refuse(); return;
        }
        int cost = (item_flags(b->kind, b->opts, *d) & 0x40000) ? 100 * next * d->min_level : next * d->value;
        cost -= static_cast<int>(RefineDb::kCostDiscount * static_cast<double>(cost));
        if (is_equipped(uid) || money_ < cost || b->rare[1] >= RefineDb::kMaxLevel ||
            d->refine_type < 1 || d->refine_type > 7 || b->rare[2] != 0 || !prop) {
            LOG_INFO("%s : hItemRefine::Send - refine Impossible (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(uid));
            refuse(); return;
        }
        // The material of this level band (sub_630380).
        const RefineMaterialRow* mr = server_.refine_db().material(d->refine_type, d->min_level);
        if (!mr) {
            LOG_INFO("%s : hItemRefine::Send - refine info is null - itemId : %u, type : %d, minLv : %d, rfLv : %d",
                     char_name_, d->id, d->refine_type, d->min_level, next);
            refuse(); return;
        }
        const uint32_t mat_id  = mr->item[next - 1];
        const int      mat_cnt = mr->cnt[next - 1];
        BagItem* mat = bag_find_uid(mat_uid);
        if (!mat || mat->item_id != mat_id || mat->count < mat_cnt || mat_cnt <= 0) {
            LOG_INFO("%s : hItemRefine::Send - material (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(mat_uid));
            refuse(); return;
        }
        // The level row (sub_630210).
        const RefineLevelRow* lr = server_.refine_db().level_row(d->refine_index);
        if (!lr) {
            LOG_INFO("%s : hItemRefine::Send - refineLvInfo is null - ItemId : %u, refineIndex : %d, rfLevel : %d",
                     char_name_, d->id, d->refine_index, next);
            refuse(); return;
        }
        // The support item (optional; an invalid one is ignored — retail).
        const RefineSupportRow* sr = nullptr;
        BagItem* sup = nullptr;
        float sup_prob = 0.0f;
        if (sup_id) {
            sr  = server_.refine_db().support(static_cast<int>(sup_id));
            sup = bag_find_uid(sup_uid);
            if (sup && sr && sr->item == sup->item_id && sup->count >= sr->count) {
                sup_prob = sr->prob;
            } else {
                LOG_INFO("%s : hItemRefine::Send - support item invalid - itemUid:%llu, InfoID:%u", char_name_,
                         static_cast<unsigned long long>(sup_uid), sup_id);
                sr = nullptr; sup = nullptr;
            }
        }
        // The roll. The fortune card's refine stat (player i16 +1492 x RefineSeed/100) is 0 here:
        // the card system does not exist yet.
        const float card_pct = 0.0f;
        double rate = static_cast<double>(lr->ratio[next - 1]);
        rate = rate + rate * (card_pct * 0.009999999776482582);
        rate = rate + sup_prob;
        if (rate >= 100.0) rate = 100.0;
        const bool success = (rate * 100.0 > static_cast<double>(rnd(10000)));
        const int16_t value = success
            ? static_cast<int16_t>(lr->accumulation[next - 1] * base->value / 100) : 0;
        money_ -= cost;

        // OnOk (sub_782F70), synchronous: the materials go, the option / the fail state, the DB.
        const BagRecord mrec = bag_decrease(*mat, static_cast<uint16_t>(mat_cnt));
        if (mrec.type == 3) server_.item_store().RemoveItem(mrec.uid);
        else                server_.item_store().SetAmount(mrec.uid, mrec.count);
        bag_apply(mrec);
        BagRecord srec;                                          // type 0: nothing (sub_601160 sends
        srec.type = 0; srec.kind = 1;                            // an empty record without a support)
        if (sup && sr) {
            sup = bag_find_uid(sup_uid);                         // (the material's record may move the bag)
            if (sup) {
                srec = bag_decrease(*sup, static_cast<uint16_t>(sr->count));
                if (srec.type == 3) server_.item_store().RemoveItem(srec.uid);
                else                server_.item_store().SetAmount(srec.uid, srec.count);
                bag_apply(srec);
            }
        }
        b = bag_find_uid(uid);                                   // (the records never touch it — safety)
        if (!b) { refuse(); return; }
        if (success) {
            const uint8_t flag = (b->rare[1] == 0) ? 1 : 2;      // insert the option / update it
            if (ItemOption* o = const_cast<ItemOption*>(item_option(*b, 11, prop))) o->value = value;
            else b->opts.push_back({ 11, prop, value, 0 });
            b->rare[0] = static_cast<uint8_t>(d->refine_type);
            b->rare[1] = static_cast<uint8_t>(next);
            server_.item_store().UpdateRefineSuccess(uid, b->rare[1], prop, value, flag);
        } else {
            b->rare[2] = 1;                                      // sub_783260(1): refine-failed
            server_.item_store().SetRefineState(uid, 1);
        }
        pet_speech(success ? 19 : 18);                           // our pet's line, before S2C 697
        LOG_DEBUG("GameClientSession %u: refine %s — item %u lv %d, option %d %+d, cost %d (rate %.1f%%)",
                  session_id(), success ? "SUCCESS" : "FAIL", d->id, b->rare[1], prop, value, cost,
                  rate);

        // S2C 697 (sub_601160; client case 697).
        PacketBuffer p(static_cast<Opcode>(0x02B9), is_paula());
        p.write<uint8_t>(success ? 1 : 0);
        write_bag_record(p, mrec);
        write_bag_record(p, srec);
        p.write<uint64_t>(uid);
        p.write<uint16_t>(static_cast<uint16_t>(prop));
        p.write<uint16_t>(static_cast<uint16_t>(value));
        p.write<uint32_t>(static_cast<uint32_t>(money_));
        send(p);
    } catch (...) { refuse(); }
}

// ── Repair (C2S 699, state 30) ───────────────────────────────────────────────

void GameClientSession::handle_repair(PacketBuffer& pkt)
{
    auto refuse = [&]() {                                        // __fputchar_106: S2C 701
        PacketBuffer p(static_cast<Opcode>(0x02BD), is_paula());
        send(p);
    };
    try {
        if (refine_ui_ != 30) return;
        const uint64_t uid     = pkt.read<uint64_t>();
        const uint64_t mat_uid = pkt.read<uint64_t>();
        BagItem* b = bag_find_uid(uid);
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d) {
            LOG_INFO("%s, hItemRepair::Send - not found (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(uid));
            refuse(); return;
        }
        // sub_782F20 + refine-failed (sub_6DE920) + not equipped.
        if (is_equipped(uid) || d->refine_type < 1 || d->refine_type > 7 ||
            b->rare[1] >= RefineDb::kMaxLevel || b->rare[2] != 1) {
            LOG_INFO("%s : hItemRepair::Send - repair impossible (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(uid));
            refuse(); return;
        }
        BagItem* mat = bag_find_uid(mat_uid);
        const ItemDef* md = mat ? server_.item_db().get(mat->item_id) : nullptr;
        const int need = b->rare[1] > 0 ? b->rare[1] : 1;        // max(1, the refine level)
        if (!mat || !md || md->sub_type != kRepairSubType || mat->count < need) {
            LOG_INFO("%s : hItemRepair::Send - not found item - material (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(mat_uid));
            refuse(); return;
        }
        // OnOk (sub_783B10): the powder goes, the fail state clears.
        const BagRecord mrec = bag_decrease(*mat, static_cast<uint16_t>(need));
        if (mrec.type == 3) server_.item_store().RemoveItem(mrec.uid);
        else                server_.item_store().SetAmount(mrec.uid, mrec.count);
        bag_apply(mrec);
        b = bag_find_uid(uid);
        if (!b) { refuse(); return; }
        b->rare[2] = 0;                                          // sub_783260(0)
        server_.item_store().SetRefineState(uid, 0);
        LOG_DEBUG("GameClientSession %u: repair — item %u (lv %d), %d x %u used", session_id(), d->id,
                  b->rare[1], need, md->id);

        // S2C 700 (sub_601330; client case 700 clears the item's fail byte).
        PacketBuffer p(static_cast<Opcode>(0x02BC), is_paula());
        p.write<uint64_t>(uid);
        write_bag_record(p, mrec);
        send(p);
    } catch (...) { refuse(); }
}

// ── Mature (growth) compound (C2S 704, state 31) ─────────────────────────────

void GameClientSession::handle_growth(PacketBuffer& pkt)
{
    auto refuse = [&]() {                                        // __fputchar_107: S2C 706
        PacketBuffer p(static_cast<Opcode>(0x02C2), is_paula());
        send(p);
    };
    try {
        if (refine_ui_ != 31) return;
        const uint64_t uid     = pkt.read<uint64_t>();
        const uint64_t mat_uid = pkt.read<uint64_t>();
        uint64_t aid_uid[2];
        aid_uid[0] = pkt.read<uint64_t>();
        aid_uid[1] = pkt.read<uint64_t>();

        // The aids (Maturing_Compound_Help; one without a row still burns — retail).
        const MaturingHelpRow* aid[2] = { nullptr, nullptr };
        for (int i = 0; i < 2; ++i) {
            if (!aid_uid[i]) continue;
            BagItem* a = bag_find_uid(aid_uid[i]);
            if (!a) {
                LOG_INFO("%s : hItemGrowthCompound::Send - item not found in inventory(itemUid:%llu)",
                         char_name_, static_cast<unsigned long long>(aid_uid[i]));
                refuse(); return;
            }
            aid[i] = server_.maturing_db().help(a->item_id);
        }
        if (aid[0] && aid[1] &&
            !((aid[0]->apply_ratio == 0.0f || aid[1]->apply_ratio == 0.0f) &&
              (aid[0]->min_ability == 0.0f || aid[1]->min_ability == 0.0f) &&
              (aid[0]->max_ability == 0.0f || aid[1]->max_ability == 0.0f))) {
            LOG_INFO("%s : hItemGrowthCompound::Send - aid item is duplicated", char_name_);
            refuse(); return;
        }
        float rate_add = 0.0f, min_mult = 0.0f, max_mult = 0.0f;
        for (int i = 0; i < 2; ++i) {
            if (!aid[i]) continue;
            rate_add += aid[i]->apply_ratio;
            min_mult += aid[i]->min_ability;
            max_mult += aid[i]->max_ability;
        }

        BagItem* b = bag_find_uid(uid);
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d) {
            LOG_INFO("%s : hItemGrowthCompound::Send - not found (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(uid));
            refuse(); return;
        }
        int cost = 5 * d->value;
        cost -= static_cast<int>(MaturingDb::kCostDiscount * static_cast<double>(cost));
        // The free slot (sub_6E8870 over the slot count byte +51).
        int slot = -1;
        {
            bool used[MaturingDb::kMaxSlots]{};
            int n = 0;
            for (const ItemOption& o : b->opts)
                if (o.type < MaturingDb::kMaxSlots) { used[o.type] = true; ++n; }
            if (n < b->rare[3])
                for (int i = 0; i < MaturingDb::kMaxSlots; ++i) if (!used[i]) { slot = i; break; }
        }
        if (is_equipped(uid) || money_ < cost || slot < 0) {
            LOG_INFO("%s : hItemGrowthCompound::Send - compound impossible (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(uid));
            refuse(); return;
        }
        if (b->rare[2] != 0 && !(item_flags(b->kind, b->opts, *d) & 0x40000)) {
            LOG_INFO("%s : hItemGrowthCompound::Send - compound impossible - by refine state (itemUid:%llu)",
                     char_name_, static_cast<unsigned long long>(uid));
            refuse(); return;
        }
        const EquipPropertyRow* eq = server_.maturing_db().equip(d->id);
        if (!eq) {
            LOG_INFO("%s : hItemGrowthCompound::Send - pCompInfo is null - itemId : %u", char_name_, d->id);
            refuse(); return;
        }
        BagItem* mat = bag_find_uid(mat_uid);
        if (!mat) {
            LOG_INFO("%s : hItemGrowthCompound::Send - not found item - material (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(mat_uid));
            refuse(); return;
        }
        const MaturingMaterialRow* mr = server_.maturing_db().material(mat->item_id);
        if (!mr || !MaturingDb::allows(*eq, mr->property) || mat->count < mr->item_num ||
            d->min_level < mr->equip_lv) {
            LOG_INFO("%s : hItemGrowthCompound::Send - invalid material (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(mat_uid));
            refuse(); return;
        }

        // The roll (sub_784030): the DA factor, the Compound Mastery (3014) multiplier, the aids.
        double da_factor = static_cast<double>(st_da_) / (static_cast<double>(st_da_ + d->min_level) + 10.0)
                         + static_cast<double>(st_da_) / 100.0;
        if (const LearnedSkill* ms = find_skill(3014)) {
            const SkillDef* sd = server_.skill_db().get(3014);
            const EsaRow* row = sd ? server_.esa_db().row(sd->esa_table, ms->level) : nullptr;
            const double ratio = row ? row->get("CompoundRatio") : 0.0;
            if (ratio > 0.0) da_factor *= ratio;
        }
        int extra_min = static_cast<int>(static_cast<double>(mr->min_ability) * min_mult);
        if (min_mult != 0.0f) {
            if (mr->min_ability > 0) { if (extra_min < 1) extra_min = 1; }
            else if (mr->min_ability < 0 && extra_min > -1) extra_min = -1;
        }
        int extra_max = static_cast<int>(static_cast<double>(mr->max_ability) * max_mult);
        if (max_mult != 0.0f) {
            if (mr->max_ability > 0) { if (extra_max < 1) extra_max = 1; }
            else if (mr->max_ability < 0 && extra_max > -1) extra_max = -1;
        }
        const int chance = static_cast<int>((static_cast<double>(eq->apply_ratio) + rate_add)
                                            * da_factor * 1.0 * 100.0);
        int16_t value;
        if (rnd(100) >= chance) {
            value = static_cast<int16_t>(extra_min + mr->min_ability);       // the floor
        } else {                                                             // the lucky curve roll
            const int k = server_.random_stat_db().curve_point(mr->import_, rnd(10000));
            if (k < 0) {
                LOG_INFO("GameClientSession %u: growth curve '%s' unknown — the floor value", session_id(),
                         mr->import_.c_str());
                value = static_cast<int16_t>(extra_min + mr->min_ability);
            } else {
                const int lo = mr->min_ability + extra_min;
                const int hi = mr->max_ability + extra_max;
                value = static_cast<int16_t>(static_cast<int>(static_cast<double>(hi - lo) / 100.0
                                                              * static_cast<double>(k)) + lo);
            }
        }
        money_ -= cost;
        const int32_t mixed = static_cast<int32_t>(mat->item_id);    // tbl_item_option mixed_item

        // OnOk (sub_784B00), synchronous: the material + the aids go, the slot option lands.
        const BagRecord mrec = bag_decrease(*mat, static_cast<uint16_t>(mr->item_num));
        if (mrec.type == 3) server_.item_store().RemoveItem(mrec.uid);
        else                server_.item_store().SetAmount(mrec.uid, mrec.count);
        bag_apply(mrec);
        BagRecord arec[2];
        for (int i = 0; i < 2; ++i) {
            arec[i].type = 0; arec[i].kind = 1;                  // type 0: no aid in that place
            if (!aid_uid[i]) continue;
            BagItem* a = bag_find_uid(aid_uid[i]);
            if (!a) continue;
            arec[i] = bag_decrease(*a, 1);
            if (arec[i].type == 3) server_.item_store().RemoveItem(arec[i].uid);
            else                   server_.item_store().SetAmount(arec[i].uid, arec[i].count);
            bag_apply(arec[i]);
        }
        b = bag_find_uid(uid);
        if (!b) { refuse(); return; }
        b->opts.push_back({ static_cast<uint32_t>(slot), static_cast<int16_t>(mr->property), value, mixed });
        server_.item_store().AddRemixOption(uid, static_cast<uint8_t>(slot),
                                            static_cast<int16_t>(mr->property), value, mixed);
        LOG_DEBUG("GameClientSession %u: mature compound — item %u slot %d, option %d %+d (chance %d%%), cost %d",
                  session_id(), d->id, slot, mr->property, value, chance, cost);

        // S2C 705 (sub_601510; client case 705 — it reads a second 9-byte property the 2012 server
        // never wrote: zeros, its first dword 0 skips it).
        PacketBuffer p(static_cast<Opcode>(0x02C1), is_paula());
        write_bag_record(p, mrec);
        p.write<uint64_t>(uid);
        p.write<uint8_t>(static_cast<uint8_t>(slot));
        p.write<uint16_t>(static_cast<uint16_t>(mr->property));
        p.write<int16_t>(value);
        p.write<uint32_t>(static_cast<uint32_t>(mixed));
        for (int i = 0; i < 9; ++i) p.write<uint8_t>(0);
        p.write<uint32_t>(static_cast<uint32_t>(money_));
        write_bag_record(p, arec[0]);
        write_bag_record(p, arec[1]);
        send(p);
    } catch (...) { refuse(); }
}

// ── The reset (C2S 734, state 32) ────────────────────────────────────────────

void GameClientSession::handle_resetmix(PacketBuffer& pkt)
{
    auto refuse = [&]() {                                        // __fputchar_112: S2C 736
        PacketBuffer p(static_cast<Opcode>(0x02E0), is_paula());
        send(p);
    };
    try {
        if (refine_ui_ != 32) return;
        const uint64_t uid     = pkt.read<uint64_t>();
        const uint64_t mat_uid = pkt.read<uint64_t>();
        const uint32_t slot    = pkt.read<uint32_t>();

        BagItem* b = bag_find_uid(uid);
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d) {
            LOG_INFO("%s : hItemGrowthCompoundReset::Send - not found (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(uid));
            refuse(); return;
        }
        if (!server_.maturing_db().equip(d->id)) {
            LOG_INFO("%s : hItemGrowthCompoundReset::Send - can't mixable item (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(uid));
            refuse(); return;
        }
        bool any = false;
        const ItemOption* at = nullptr;
        for (const ItemOption& o : b->opts)
            if (o.type < MaturingDb::kMaxSlots) { any = true; if (o.type == slot) at = &o; }
        if (!any) {
            LOG_INFO("%s : hItemGrowthCompoundReset::Send - mix slot is empty (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(uid));
            refuse(); return;
        }
        if (is_equipped(uid)) {
            LOG_INFO("%s : hItemGrowthCompoundReset::Send - item is equiped (itemUid:%llu)", char_name_,
                     static_cast<unsigned long long>(uid));
            refuse(); return;
        }
        BagItem* mat = bag_find_uid(mat_uid);
        if (!mat || mat->item_id != kResetMaterial) {
            LOG_INFO("%s : hItemGrowthCompoundReset::Send - not proper material item (itemUid:%llu)",
                     char_name_, static_cast<unsigned long long>(mat_uid));
            refuse(); return;
        }
        if (slot >= MaturingDb::kMaxSlots || !at) {
            LOG_INFO("%s : hItemGrowthCompoundReset::Send - %u slot is NOT empty (itemUid:%llu)", char_name_,
                     slot, static_cast<unsigned long long>(uid));
            refuse(); return;
        }

        // OnOk (sub_787B20): the bottle goes, the slot option goes.
        const BagRecord mrec = bag_decrease(*mat, 1);
        if (mrec.type == 3) server_.item_store().RemoveItem(mrec.uid);
        else                server_.item_store().SetAmount(mrec.uid, mrec.count);
        bag_apply(mrec);
        b = bag_find_uid(uid);
        if (!b) { refuse(); return; }
        for (auto it = b->opts.begin(); it != b->opts.end(); ++it)
            if (it->type == slot) { b->opts.erase(it); break; }              // sub_6E7CE0
        server_.item_store().DeleteRemixOption(uid, static_cast<uint8_t>(slot));
        LOG_DEBUG("GameClientSession %u: reset mix — item %u slot %u", session_id(), d->id, slot);

        // S2C 735 (sub_601A20; client case 735 — it reads a SECOND slot the 2012 server never
        // wrote: 0 skips it).
        PacketBuffer p(static_cast<Opcode>(0x02DF), is_paula());
        p.write<uint64_t>(uid);
        p.write<uint32_t>(slot);
        p.write<uint32_t>(0);
        write_bag_record(p, mrec);
        send(p);
    } catch (...) { refuse(); }
}
