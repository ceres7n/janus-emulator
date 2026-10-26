#pragma once
#include <cstdint>
#include <vector>
#include "database.hpp"

// One inventory row (char_inventory). Mirrors Trickster's item instance: stack +
// equip/bag slot + durability/refine + 4 option slots (crafting).
struct InvItem {
    uint64_t uid{ 0 };
    uint32_t item_id{ 0 };
    uint16_t amount{ 1 };
    int16_t  bag_slot{ -1 };
    int8_t   equip_slot{ -1 };
    uint8_t  where{ 1 };           // item_where: 1 the bag, 2 / 21..29 a warehouse tab
    uint16_t durability{ 100 };
    uint8_t  refine{ 0 };          // tbl_item.refine_level (byte +49 on the wire)
    uint8_t  refine_state{ 0 };    // tbl_item.refine_state: 1 = a failed refine (byte +50)
    int16_t  refine_type{ 0 };     // tbl_item.refine_type: the refined option index (10000..10012)
    uint8_t  item_slot{ 0 };       // a rare item's compound slot count (Paula tbl_item.item_slot)
    int16_t  opt1_id{ 0 }; int32_t opt1_val{ 0 };
    int16_t  opt2_id{ 0 }; int32_t opt2_val{ 0 };
    int16_t  opt3_id{ 0 }; int32_t opt3_val{ 0 };
    int16_t  opt4_id{ 0 }; int32_t opt4_val{ 0 };
};

// One option of an item (char_item_option = Paula tbl_item_option): option_type 0~9 compound slot,
// 10 base (+random) stat, 11 refine, 12~ others.
struct ItemOptionRow {
    uint64_t item_uid{ 0 };
    uint8_t  type{ 10 };
    int16_t  index{ 0 };
    int16_t  value{ 0 };
    int32_t  mixed_item{ 0 };
};

// ItemStore — character inventory data access (procs→methods, like CharStore).
// Backed by the char_inventory table; the static item master is item_db / item_db.yml.
class ItemStore {
public:
    explicit ItemStore(Database& db) : db_(db) {}

    // The bag (item_where 1; the equipped items are in it with their equip_slot).
    std::vector<InvItem> LoadInventory(uint32_t char_uid);
    // uspm_item_select_by_where for one warehouse tab (item_where 2 / 21..29), by item uid.
    std::vector<InvItem> LoadWhere(uint32_t char_uid, uint8_t where);
    // The options of every item the character owns (uspm_item_option_select_by_where).
    std::vector<ItemOptionRow> LoadItemOptions(uint32_t char_uid);
    // uspm_item_select_by_where 3: the account's bank (item_where 3, user_uid; the first 24 by item uid).
    std::vector<InvItem> LoadBank(uint32_t user_uid);
    // uspm_item_option_select_by_where 3: the options of the account's bank items.
    std::vector<ItemOptionRow> LoadBankOptions(uint32_t user_uid);
    // uspm_item_char_update_move_inven_to_bank: an item moving between the bag (1) and the account's
    // bank (3) — the same flags as MoveItem; a rare item / a whole stack changes owner and place (the
    // bag -> the bank also sets a whole common stack's count, the way back does not), 0/2 joins
    // to_uid's stack, 2/1 splits a new stack of to_attr off (new_uid), 2/2 sets both. 0 = done, else
    // the procedure's error (101..110 the bag -> the bank, 201..210 back; 99 another pair of places).
    int MoveBankItem(uint32_t user_uid, uint32_t char_uid, uint32_t item_id, uint8_t item_class,
                     uint8_t from_where, uint64_t from_uid, uint16_t from_attr, uint8_t from_flag,
                     uint8_t to_where, uint64_t to_uid, uint16_t to_attr, uint8_t to_flag, uint64_t& new_uid);

    // uspm_item_char_update_move_inven_to_warehouse: an item moving between two places (the bag 1, a
    // warehouse tab 2 / 21..29). A rare item (class 2) only changes its place; a common one (class 1)
    // by the update flags (0 delete, 1 insert, 2 update; attr = the final count): 0/1 the whole stack
    // moves (its count set first), 0/2 it joins the stack to_uid, 2/1 a new stack of to_attr is split
    // off (new_uid), 2/2 both counts change. 0 = done, else the procedure's error (101..112).
    int MoveItem(uint32_t char_uid, uint32_t item_id, uint8_t item_class,
                 uint8_t from_where, uint64_t from_uid, uint16_t from_attr, uint8_t from_flag,
                 uint8_t to_where, uint64_t to_uid, uint16_t to_attr, uint8_t to_flag, uint64_t& new_uid);

    // USPM_ItemTrade_User (LogCode 30, the 1:1 trade): each side's records {ItemUID, ItemIndex, ItemClass,
    // LastCount, UpdateFlag} — a common item (class 1): 0 leaves the character (Char_UID 0), 2 its count
    // becomes LastCount, 1 a new stack in the bag (its uid comes back in new_uid); a rare item (class 2): 1
    // changes owner (0 is implicit) — and each side's final gelders (< 0: unchanged), in one transaction.
    // 0 = done, else the procedure's error (101 money, 102 insert, 103 rows differ — rolled back).
    struct TradeRow { uint64_t uid{ 0 }; uint32_t item_id{ 0 }; uint8_t item_class{ 1 }; uint16_t count{ 0 };
                      uint8_t flag{ 0 }; uint64_t new_uid{ 0 }; };
    int TradeItems(uint32_t first_char, std::vector<TradeRow>& first, int32_t first_money,
                   uint32_t second_char, std::vector<TradeRow>& second, int32_t second_money);

    // Insert one item into the bag. bag_slot<0 → next free bag slot (>=100). Returns the row uid (0 on fail).
    uint64_t AddItem(uint32_t char_uid, uint32_t item_id, uint16_t amount, int16_t bag_slot = -1);
    // uspm_item_insert_get_item_rare: a new rare item with its compound slot count and its options
    // (usp_item_option_insert, option type 10; rows with an index <= 0 are skipped). Returns the uid.
    uint64_t AddRareItem(uint32_t char_uid, uint32_t item_id, uint16_t amount, uint8_t item_slot,
                         const std::vector<ItemOptionRow>& options);

    // uspm_item_delete_item: the item and its options.
    bool RemoveItem(uint64_t uid);
    bool SetAmount(uint64_t uid, uint16_t amount);
    bool SetSlots(uint64_t uid, int16_t bag_slot, int8_t equip_slot);
    // uspm_item_char_update_move_inven_to_equip / ..._exchange_inven_to_equip: the item worn in
    // equip_slot (0..22, item_where 0) or back in the bag (-1, item_where 1).
    bool SetEquipSlot(uint64_t uid, int8_t equip_slot);
    // uspm_item_update_uid_item_rare: a rare item dropped on the ground belongs to nobody
    // (char_uid 0, drop_flag 0) until someone picks it up (drop_flag 1 -> the picker).
    bool SetOwner(uint64_t uid, uint32_t char_uid);

    // uspm_item_update_refine_success (the item consumption + money travel the usual paths):
    // refine_type / refine_level on the item and the type-11 option row (flag 1 insert, 2 update).
    bool UpdateRefineSuccess(uint64_t uid, uint8_t refine_level, int16_t option_index,
                             int16_t option_value, uint8_t option_flag);
    // uspm_item_update_refine_state: 1 = the refine failed, 0 = a Repair Powder fixed it.
    bool SetRefineState(uint64_t uid, uint8_t state);
    // uspm_item_update_remix_success: a mature compound's slot option (option_type = the slot 0..9,
    // mixed_item = the material's item id).
    bool AddRemixOption(uint64_t uid, uint8_t slot, int16_t option_index, int16_t option_value,
                        int32_t mixed_item);
    // uspm_item_option_delete_remix: the slot option goes.
    bool DeleteRemixOption(uint64_t uid, uint8_t slot);

private:
    Database& db_;
};
