#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "database.hpp"
#include "store/char_record.hpp"

// CharStore — persistent character data access, mirroring the MasterServer
// `cCharStore` (reverse/specs/01_masterserver_char.md). Each method is the C++
// equivalent of one stored procedure (procs → methods), run against MariaDB/MySQL.
// Lives on the MasterServer, as in the real split: the LoginServer reaches it through the
// Login↔Master relay (MREQ_CHAR_*).
class CharStore {
public:
    explicit CharStore(Database& db) : db_(db) {}

    // cUserStore::GetByOwnerFromDb (MasterServer sub_47A4D0) + uspm_char_all_select_char_slot_info
    // (Paula): the account's characters for the char-select screen (slot < usable_slot), with the
    // looks of their equipped items; `usable_slot` = the account's slot count (user_info.usable_slot).
    std::vector<CharRecord> LoadCharsByOwner(uint32_t owner, uint8_t& usable_slot);

    // name uniqueness among ACTIVE chars (deleted chars free their name).
    bool NameExists(const std::string& name);

    // cCharStore::InsertIntoDb (MasterServer sub_44C070) + usp_insert_character (Paula): the checks,
    // then the character with its starter kit. Returns the retail result: 0 = made (chr gets its
    // uid and its equipped looks), 9999 bad growth points, 1020 no slot left, 1023 name taken,
    // 1019 name not allowed, 9001 / 105 / 101..103 a write failed (nothing is kept).
    int MakeNewCharacter(CharRecord& chr);

    // uspm_deleted_char_insert (Paula): the name freed ('0000000000000017+'), authority 2 (+ our deleted=1),
    // tbl_deleted_char with the client's IP, the friends and the MyCamp cell / walls gone. Returns the
    // procedure's result: 0, 8001 no such character, 8003 / 8002 a write failed (nothing kept).
    int DeleteFromDb(uint32_t charUid, uint32_t owner, const std::string& user_ip);

    // uspm_char_state_update_* — persist mutable in-world progress (position + money/level/
    // exp/hp/mp) on zone-leave / logout. Returns true if a row changed.
    bool SaveState(uint32_t uid, int16_t zone, int32_t pos_x, int32_t pos_y,
                   int32_t money, int16_t level, int32_t exp, int32_t hp, int32_t mp);

    // Persist the 12 per-stat accumulated points (CANONICAL order: ap, ac, dx, max_mp, ma, md,
    // max_wt, da, lk, max_hp, dp, hv — the retail +764..+786 slots, serializer sub_75CB40), the
    // unspent pool (attr_pts) and the TM level. Returns true if a row changed.
    bool SaveStatPoints(uint32_t uid, const int32_t accpts[12], int32_t attr_pts, int32_t tm_level,
                        int32_t tm_exp, int32_t tm_point);

    // Persist the return town (characters.home; retail Player +1656). Returns true if a row changed.
    bool SaveHome(uint32_t uid, int16_t home);

    // uspm_insert_char_names_changed (Paula), its name part (the caller uses the item up — the procedure's own
    // last step): 1023 the name taken (tbl_sleep_char: no hibernation here), 1019 not allowed
    // (fn_CharacterName_ValidCheck); then the name (101 unless one row) and its log char_names {uid, old, new,
    // now, "by item using(index)", old} (102). 0 = renamed (old_name = the name before).
    int RenameCharacter(uint32_t uid, const std::string& new_name, uint32_t item_index, std::string& old_name);

private:
    Database& db_;
};
