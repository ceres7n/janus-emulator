#include "store/item_store.hpp"
#include <cstdio>
#include <string>

namespace {

std::vector<InvItem> inv_rows(const DBResult& rows)
{
    std::vector<InvItem> out;
    for (auto& r : rows) {
        InvItem it{};
        it.uid        = std::stoull(r.at("uid"));
        it.item_id    = static_cast<uint32_t>(std::stoul(r.at("item_id")));
        it.amount     = static_cast<uint16_t>(std::stoi(r.at("amount")));
        it.bag_slot   = static_cast<int16_t>(std::stoi(r.at("bag_slot")));
        it.equip_slot = static_cast<int8_t>(std::stoi(r.at("equip_slot")));
        it.where      = static_cast<uint8_t>(std::stoi(r.at("item_where")));
        it.durability = static_cast<uint16_t>(std::stoi(r.at("durability")));
        it.refine     = static_cast<uint8_t>(std::stoi(r.at("refine")));
        it.refine_state = static_cast<uint8_t>(std::stoi(r.at("refine_state")));
        it.refine_type  = static_cast<int16_t>(std::stoi(r.at("refine_type")));
        it.item_slot  = static_cast<uint8_t>(std::stoi(r.at("item_slot")));
        it.opt1_id = static_cast<int16_t>(std::stoi(r.at("opt1_id"))); it.opt1_val = std::stoi(r.at("opt1_val"));
        it.opt2_id = static_cast<int16_t>(std::stoi(r.at("opt2_id"))); it.opt2_val = std::stoi(r.at("opt2_val"));
        it.opt3_id = static_cast<int16_t>(std::stoi(r.at("opt3_id"))); it.opt3_val = std::stoi(r.at("opt3_val"));
        it.opt4_id = static_cast<int16_t>(std::stoi(r.at("opt4_id"))); it.opt4_val = std::stoi(r.at("opt4_val"));
        out.push_back(it);
    }
    return out;
}

} // namespace

std::vector<InvItem> ItemStore::LoadInventory(uint32_t char_uid)
{
    return inv_rows(db_.query(
        "SELECT uid, item_id, amount, bag_slot, equip_slot, item_where, durability, refine, refine_state, refine_type, item_slot, "
        "       opt1_id, opt1_val, opt2_id, opt2_val, opt3_id, opt3_val, opt4_id, opt4_val "
        "FROM char_inventory WHERE char_uid=%u AND item_where=1 ORDER BY bag_slot",
        char_uid
    ));
}

std::vector<InvItem> ItemStore::LoadWhere(uint32_t char_uid, uint8_t where)
{
    return inv_rows(db_.query(
        "SELECT uid, item_id, amount, bag_slot, equip_slot, item_where, durability, refine, refine_state, refine_type, item_slot, "
        "       opt1_id, opt1_val, opt2_id, opt2_val, opt3_id, opt3_val, opt4_id, opt4_val "
        "FROM char_inventory WHERE char_uid=%u AND item_where=%u ORDER BY uid",
        char_uid, static_cast<unsigned>(where)
    ));
}

int ItemStore::MoveItem(uint32_t char_uid, uint32_t item_id, uint8_t item_class,
                        uint8_t from_where, uint64_t from_uid, uint16_t from_attr, uint8_t from_flag,
                        uint8_t to_where, uint64_t to_uid, uint16_t to_attr, uint8_t to_flag, uint64_t& new_uid)
{
    const unsigned long long fu = static_cast<unsigned long long>(from_uid);
    const unsigned long long tu = static_cast<unsigned long long>(to_uid);
    new_uid = 0;
    // (statement by statement — the procedure's transaction is not reproduced)
    if (item_class == 2) {
        if (db_.execute("UPDATE char_inventory SET item_where=%u WHERE uid=%llu AND item_where=%u",
                        static_cast<unsigned>(to_where), fu, static_cast<unsigned>(from_where)) != 1) return 101;
        return 0;
    }
    if (item_class != 1) return 0;
    if (from_flag == 0 && to_flag == 1) {
        if (db_.execute("UPDATE char_inventory SET amount=%u WHERE uid=%llu", to_attr, fu) != 1) {
            // (MySQL counts a row whose value did not change as not affected: check it is there)
            if (db_.query("SELECT uid FROM char_inventory WHERE uid=%llu", fu).size() != 1) return 112;
        }
        if (db_.execute("UPDATE char_inventory SET item_where=%u WHERE uid=%llu AND item_where=%u",
                        static_cast<unsigned>(to_where), fu, static_cast<unsigned>(from_where)) != 1) return 102;
        return 0;
    }
    if (from_flag == 0 && to_flag == 2) {
        db_.execute("DELETE FROM char_item_option WHERE item_uid=%llu", fu);
        if (db_.execute("DELETE FROM char_inventory WHERE uid=%llu AND item_where=%u", fu,
                        static_cast<unsigned>(from_where)) != 1) return 103;
        if (db_.execute("UPDATE char_inventory SET amount=%u WHERE uid=%llu", to_attr, tu) != 1) return 105;
        return 0;
    }
    if (from_flag == 2 && to_flag == 1) {
        if (db_.execute("UPDATE char_inventory SET amount=%u WHERE uid=%llu", from_attr, fu) != 1) return 106;
        int bag_slot = -1;
        if (to_where == 1) {
            auto rows = db_.query(
                "SELECT COALESCE(MAX(bag_slot), 99) + 1 AS nxt FROM char_inventory WHERE char_uid=%u", char_uid);
            bag_slot = rows.empty() ? 100 : std::stoi(rows[0].at("nxt"));
            if (bag_slot < 100) bag_slot = 100;
        }
        new_uid = db_.insert(
            "INSERT INTO char_inventory (char_uid, item_id, amount, bag_slot, equip_slot, item_where) "
            "VALUES (%u, %u, %u, %d, -1, %u)",
            char_uid, item_id, to_attr, bag_slot, static_cast<unsigned>(to_where));
        if (!new_uid) return 107;
        return 0;
    }
    if (from_flag == 2 && to_flag == 2) {
        if (db_.execute("UPDATE char_inventory SET amount=%u WHERE uid=%llu", from_attr, fu) != 1) return 108;
        if (db_.execute("UPDATE char_inventory SET amount=%u WHERE uid=%llu", to_attr, tu) != 1) return 109;
        return 0;
    }
    return 110;
}

namespace {

std::vector<ItemOptionRow> opt_rows(const DBResult& rows)
{
    std::vector<ItemOptionRow> out;
    for (auto& r : rows) {
        ItemOptionRow o;
        o.item_uid   = std::stoull(r.at("item_uid"));
        o.type       = static_cast<uint8_t>(std::stoi(r.at("option_type")));
        o.index      = static_cast<int16_t>(std::stoi(r.at("option_index")));
        o.value      = static_cast<int16_t>(std::stoi(r.at("option_value")));
        o.mixed_item = std::stoi(r.at("mixed_item"));
        out.push_back(o);
    }
    return out;
}

} // namespace

std::vector<ItemOptionRow> ItemStore::LoadItemOptions(uint32_t char_uid)
{
    return opt_rows(db_.query(
        "SELECT o.item_uid, o.option_type, o.option_index, o.option_value, o.mixed_item "
        "FROM char_item_option o JOIN char_inventory i ON i.uid = o.item_uid "
        "WHERE i.char_uid=%u ORDER BY o.item_uid, o.option_type, o.option_index",
        char_uid
    ));
}

std::vector<InvItem> ItemStore::LoadBank(uint32_t user_uid)
{
    return inv_rows(db_.query(
        "SELECT uid, item_id, amount, bag_slot, equip_slot, item_where, durability, refine, refine_state, refine_type, item_slot, "
        "       opt1_id, opt1_val, opt2_id, opt2_val, opt3_id, opt3_val, opt4_id, opt4_val "
        "FROM char_inventory WHERE user_uid=%u AND item_where=3 ORDER BY uid LIMIT 24",
        user_uid
    ));
}

std::vector<ItemOptionRow> ItemStore::LoadBankOptions(uint32_t user_uid)
{
    return opt_rows(db_.query(
        "SELECT o.item_uid, o.option_type, o.option_index, o.option_value, o.mixed_item "
        "FROM char_item_option o JOIN char_inventory i ON i.uid = o.item_uid "
        "WHERE i.user_uid=%u AND i.item_where=3 ORDER BY o.item_uid, o.option_type, o.option_index",
        user_uid
    ));
}

int ItemStore::MoveBankItem(uint32_t user_uid, uint32_t char_uid, uint32_t item_id, uint8_t item_class,
                            uint8_t from_where, uint64_t from_uid, uint16_t from_attr, uint8_t from_flag,
                            uint8_t to_where, uint64_t to_uid, uint16_t to_attr, uint8_t to_flag, uint64_t& new_uid)
{
    const unsigned long long fu = static_cast<unsigned long long>(from_uid);
    const unsigned long long tu = static_cast<unsigned long long>(to_uid);
    new_uid = 0;
    int base = 0;
    if (from_where == 1 && to_where == 3)      base = 100;
    else if (from_where == 3 && to_where == 1) base = 200;
    else return 0;                                       // (any other pair: the procedure does nothing)
    const bool to_bank = base == 100;
    auto next_bag_slot = [&]() {
        auto rows = db_.query("SELECT COALESCE(MAX(bag_slot), 99) + 1 AS nxt FROM char_inventory WHERE char_uid=%u",
                              char_uid);
        int s = rows.empty() ? 100 : std::stoi(rows[0].at("nxt"));
        return s < 100 ? 100 : s;
    };
    // The row changes owner and place (tbl_item_user / tbl_item_char: the insert, then the other's
    // delete — exactly one row): the bag -> the bank gives it the account, the way back the character.
    auto move_row = [&]() {
        if (to_bank)
            return db_.execute("UPDATE char_inventory SET item_where=3, char_uid=0, user_uid=%u, equip_slot=-1 "
                               "WHERE uid=%llu AND item_where=1", user_uid, fu) == 1;
        return db_.execute("UPDATE char_inventory SET item_where=1, char_uid=%u, user_uid=0, bag_slot=%d, equip_slot=-1 "
                           "WHERE uid=%llu AND item_where=3", char_uid, next_bag_slot(), fu) == 1;
    };
    // (MySQL counts a row whose value did not change as not affected: check it is there)
    auto set_amount = [&](unsigned long long uid, uint16_t v) {
        if (db_.execute("UPDATE char_inventory SET amount=%u WHERE uid=%llu", v, uid) == 1) return true;
        return db_.query("SELECT uid FROM char_inventory WHERE uid=%llu", uid).size() == 1;
    };
    // (statement by statement — the procedure's transaction is not reproduced)
    if (item_class == 2) return move_row() ? 0 : base + 1;
    if (item_class != 1) return 0;
    if (from_flag == 0 && to_flag == 1) {
        if (!move_row()) return base + 1;
        // the bag -> the bank only: the stack's final count (the 2011 patch: a cached count, e.g. used
        // potions, would otherwise come back whole)
        if (to_bank && !set_amount(fu, to_attr)) return 112;
        return 0;
    }
    if (from_flag == 0 && to_flag == 2) {
        db_.execute("DELETE FROM char_item_option WHERE item_uid=%llu", fu);
        if (db_.execute("DELETE FROM char_inventory WHERE uid=%llu AND item_where=%u", fu,
                        static_cast<unsigned>(from_where)) != 1) return base + 3;
        if (!set_amount(tu, to_attr)) return base + 5;
        return 0;
    }
    if (from_flag == 2 && to_flag == 1) {
        if (!set_amount(fu, from_attr)) return base + 6;
        new_uid = to_bank
            ? db_.insert("INSERT INTO char_inventory (char_uid, user_uid, item_id, amount, bag_slot, equip_slot, "
                         "item_where) VALUES (0, %u, %u, %u, -1, -1, 3)", user_uid, item_id, to_attr)
            : db_.insert("INSERT INTO char_inventory (char_uid, user_uid, item_id, amount, bag_slot, equip_slot, "
                         "item_where) VALUES (%u, 0, %u, %u, %d, -1, 1)", char_uid, item_id, to_attr, next_bag_slot());
        if (!new_uid) return base + 7;
        return 0;
    }
    if (from_flag == 2 && to_flag == 2) {
        if (!set_amount(fu, from_attr)) return base + 8;
        if (!set_amount(tu, to_attr)) return base + 9;
        return 0;
    }
    return base + 10;
}

uint64_t ItemStore::AddItem(uint32_t char_uid, uint32_t item_id, uint16_t amount, int16_t bag_slot)
{
    if (bag_slot < 0) {
        // next free bag slot (bag slots start at 100 per the schema convention)
        auto rows = db_.query(
            "SELECT COALESCE(MAX(bag_slot), 99) + 1 AS nxt FROM char_inventory WHERE char_uid=%u",
            char_uid);
        bag_slot = rows.empty() ? 100 : static_cast<int16_t>(std::stoi(rows[0].at("nxt")));
        if (bag_slot < 100) bag_slot = 100;
    }
    return db_.insert(
        "INSERT INTO char_inventory (char_uid, item_id, amount, bag_slot, equip_slot) "
        "VALUES (%u, %u, %u, %d, -1)",
        char_uid, item_id, amount, bag_slot
    );
}

uint64_t ItemStore::AddRareItem(uint32_t char_uid, uint32_t item_id, uint16_t amount, uint8_t item_slot,
                                const std::vector<ItemOptionRow>& options)
{
    auto rows = db_.query(
        "SELECT COALESCE(MAX(bag_slot), 99) + 1 AS nxt FROM char_inventory WHERE char_uid=%u", char_uid);
    int bag_slot = rows.empty() ? 100 : std::stoi(rows[0].at("nxt"));
    if (bag_slot < 100) bag_slot = 100;
    const uint64_t uid = db_.insert(
        "INSERT INTO char_inventory (char_uid, item_id, amount, bag_slot, equip_slot, item_slot) "
        "VALUES (%u, %u, %u, %d, -1, %u)",
        char_uid, item_id, amount, bag_slot, item_slot);
    if (!uid) return 0;
    // usp_item_option_insert: one row per option whose index is > 0.
    std::string values;
    for (const ItemOptionRow& o : options) {
        if (o.index <= 0) continue;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s(%llu,%u,%d,%d,%d)", values.empty() ? "" : ",",
                      static_cast<unsigned long long>(uid), o.type, o.index, o.value, o.mixed_item);
        values += buf;
    }
    if (!values.empty())
        db_.execute("INSERT INTO char_item_option (item_uid, option_type, option_index, option_value, mixed_item) "
                    "VALUES %s", values.c_str());
    return uid;
}

bool ItemStore::RemoveItem(uint64_t uid)
{
    db_.execute("DELETE FROM char_item_option WHERE item_uid=%llu", static_cast<unsigned long long>(uid));
    return db_.execute("DELETE FROM char_inventory WHERE uid=%llu",
                       static_cast<unsigned long long>(uid)) > 0;
}

bool ItemStore::SetAmount(uint64_t uid, uint16_t amount)
{
    if (amount == 0) return RemoveItem(uid);
    return db_.execute("UPDATE char_inventory SET amount=%u WHERE uid=%llu",
                       amount, static_cast<unsigned long long>(uid)) > 0;
}

bool ItemStore::SetSlots(uint64_t uid, int16_t bag_slot, int8_t equip_slot)
{
    return db_.execute("UPDATE char_inventory SET bag_slot=%d, equip_slot=%d WHERE uid=%llu",
                       bag_slot, static_cast<int>(equip_slot),
                       static_cast<unsigned long long>(uid)) > 0;
}

bool ItemStore::SetEquipSlot(uint64_t uid, int8_t equip_slot)
{
    return db_.execute("UPDATE char_inventory SET equip_slot=%d WHERE uid=%llu", static_cast<int>(equip_slot),
                       static_cast<unsigned long long>(uid)) > 0;
}

bool ItemStore::SetOwner(uint64_t uid, uint32_t char_uid)
{
    return db_.execute("UPDATE char_inventory SET char_uid=%u, equip_slot=-1 WHERE uid=%llu",
                       char_uid, static_cast<unsigned long long>(uid)) > 0;
}

// uspm_item_update_refine_success: refine_type (the option index) + refine_level on the item, the
// type-11 option row inserted (flag 1) or set to the new accumulated value (flag 2). The log tables
// (tbl_item_refine_log) are not reproduced; the consumed items and the money go the usual ways.
bool ItemStore::UpdateRefineSuccess(uint64_t uid, uint8_t refine_level, int16_t option_index,
                                    int16_t option_value, uint8_t option_flag)
{
    // (execute() throws on a failed statement; an unchanged row is not a failure)
    const unsigned long long u = static_cast<unsigned long long>(uid);
    db_.execute("UPDATE char_inventory SET refine=%u, refine_type=%d WHERE uid=%llu",
                static_cast<unsigned>(refine_level), static_cast<int>(option_index), u);
    if (option_flag == 1)
        return db_.execute("INSERT INTO char_item_option (item_uid, option_type, option_index, option_value, "
                           "mixed_item) VALUES (%llu, 11, %d, %d, 0)",
                           u, static_cast<int>(option_index), static_cast<int>(option_value)) > 0;
    db_.execute("UPDATE char_item_option SET option_value=%d WHERE item_uid=%llu AND option_type=11 "
                "AND option_index=%d",
                static_cast<int>(option_value), u, static_cast<int>(option_index));
    return true;
}

// uspm_item_update_refine_state: 1 = the refine failed, 0 = repaired.
bool ItemStore::SetRefineState(uint64_t uid, uint8_t state)
{
    db_.execute("UPDATE char_inventory SET refine_state=%u WHERE uid=%llu",
                static_cast<unsigned>(state), static_cast<unsigned long long>(uid));
    return true;
}

// uspm_item_update_remix_success: the new slot option (option_type = the slot, mixed_item = the
// material's item id).
bool ItemStore::AddRemixOption(uint64_t uid, uint8_t slot, int16_t option_index, int16_t option_value,
                               int32_t mixed_item)
{
    return db_.execute("INSERT INTO char_item_option (item_uid, option_type, option_index, option_value, "
                       "mixed_item) VALUES (%llu, %u, %d, %d, %d)",
                       static_cast<unsigned long long>(uid), static_cast<unsigned>(slot),
                       static_cast<int>(option_index), static_cast<int>(option_value), mixed_item) > 0;
}

// uspm_item_option_delete_remix.
bool ItemStore::DeleteRemixOption(uint64_t uid, uint8_t slot)
{
    return db_.execute("DELETE FROM char_item_option WHERE item_uid=%llu AND option_type=%u",
                       static_cast<unsigned long long>(uid), static_cast<unsigned>(slot)) > 0;
}

// USPM_ItemTrade_User (DBReader\procs_paula; LogCode 30 = the 1:1 trade). In one transaction: each side's final
// gelders (< 0 = unchanged; error 101 when the character is not there), then the item records — a common item
// (class 1): flag 0 leaves the character (Char_UID = 0, the row is kept), 2 takes the count LastCount, 1 is a new
// stack in the bag (Item_Where 1; its new uid comes back); a rare item (class 2): 1 changes owner (its uid, slots
// and options travel), 0 is implicit. The rows touched must match the records (each rare insert counts twice, its
// owner's delete inside it), else error 103 and nothing is kept. (Not here: the unique items' duty bookkeeping —
// Tbl_Unique_Item_Info / Tbl_Unique_Char_Duty_Info — these tables are not in our schema.)
int ItemStore::TradeItems(uint32_t first_char, std::vector<TradeRow>& first, int32_t first_money,
                          uint32_t second_char, std::vector<TradeRow>& second, int32_t second_money)
{
    DBTransaction tx(db_);
    if (first_money >= 0 || second_money >= 0) {
        if (first_money >= 0) {
            db_.execute("UPDATE characters SET money=%d WHERE uid=%u", first_money, first_char);
            if (db_.query("SELECT uid FROM characters WHERE uid=%u", first_char).size() != 1) return 101;
        }
        if (second_money >= 0) {
            db_.execute("UPDATE characters SET money=%d WHERE uid=%u", second_money, second_char);
            if (db_.query("SELECT uid FROM characters WHERE uid=%u", second_char).size() != 1) return 101;
        }
    }
    size_t rows = 0, done = 0;
    auto next_slot = [&](uint32_t char_uid) {
        auto r = db_.query("SELECT COALESCE(MAX(bag_slot), 99) + 1 AS nxt FROM char_inventory WHERE char_uid=%u",
                           char_uid);
        int s = r.empty() ? 100 : std::stoi(r[0].at("nxt"));
        return s < 100 ? 100 : s;
    };
    auto side = [&](uint32_t char_uid, std::vector<TradeRow>& list) -> bool {
        for (TradeRow& e : list) {
            ++rows;
            const unsigned long long u = static_cast<unsigned long long>(e.uid);
            if (e.item_class == 1 && e.flag == 0) {
                // (the proc leaves the row with Char_UID 0; the bag slot goes with it)
                if (db_.execute("UPDATE char_inventory SET char_uid=0, bag_slot=-1 WHERE uid=%llu AND char_uid=%u",
                                u, char_uid) == 1) ++done;
            } else if (e.item_class == 1 && e.flag == 2) {
                db_.execute("UPDATE char_inventory SET amount=%u WHERE uid=%llu", static_cast<unsigned>(e.count), u);
                if (db_.query("SELECT uid FROM char_inventory WHERE uid=%llu AND amount=%u", u,
                              static_cast<unsigned>(e.count)).size() == 1) ++done;
            } else if (e.item_class == 1 && e.flag == 1) {
                e.new_uid = db_.insert(
                    "INSERT INTO char_inventory (char_uid, item_id, amount, bag_slot, equip_slot, item_where) "
                    "VALUES (%u, %u, %u, %d, -1, 1)",
                    char_uid, e.item_id, static_cast<unsigned>(e.count), next_slot(char_uid));
                if (!e.new_uid) return false;                     // 102 'INSERT Tbl_Item_Char Fail'
                ++done;
            } else if (e.item_class == 2 && e.flag == 1) {
                if (db_.execute("UPDATE char_inventory SET char_uid=%u, bag_slot=%d, item_where=1, equip_slot=-1 "
                                "WHERE uid=%llu", char_uid, next_slot(char_uid), u) == 1) done += 2;
            }
        }
        return true;
    };
    if (!side(first_char, first) || !side(second_char, second)) return 102;
    if (rows != done) return 103;                                 // 'updated rows of items have different'
    tx.commit();
    return 0;
}
