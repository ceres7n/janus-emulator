#pragma once
#include <cstdint>

// Character record — mirrors the char_attr + char_state tables, shared by the
// LoginServer (char-select) and the MasterServer CharStore. See
// reverse/specs/01_masterserver_char.md.
struct CharRecord {
    uint32_t uid{ 0 };
    uint32_t owner{ 0 };      // account_id
    uint8_t  slot{ 0 };
    char     name[18]{ 0 };   // 17 chars + null

    int16_t  level{ 1 };

    int16_t  type{ 0 };       // race / base type
    int16_t  job{ 0 };
    int16_t  type_2{ 0 };
    uint8_t  job_2{ 0 };
    int16_t  type_3{ 0 };
    uint8_t  job_3{ 0 };
    uint8_t  f_type{ 0 };     // PMSC: 0=Power 1=Magic 2=Sense 3=Charm
    uint8_t  hair{ 0 };
    uint8_t  hair_2{ 0 };     // the 2nd / 3rd tier's hair
    uint8_t  hair_3{ 0 };
    uint8_t  job_state{ 1 };  // the tier shown: 1 = type/job/hair, 2 = type_2/job_2/hair_2, 3 = the 3rd

    uint8_t  pts_p{ 0 }, pts_m{ 0 }, pts_s{ 0 }, pts_c{ 0 };

    uint8_t  state{ 0 };
    uint8_t  authority{ 0 };
    int16_t  zone{ 0 };
    int32_t  pos_x{ 0 };
    int32_t  pos_y{ 0 };
    int16_t  home{ 0 };
    int32_t  money{ 0 };

    uint8_t  deleted{ 0 };            // 0=active, 1=frozen/soft-deleted
    char     freeze_until[20]{ 0 };

    // The char-select entry's other columns (MasterServer cUserStore::GetByOwnerFromDb sub_47A4D0 /
    // cCharStore::InsertIntoDb sub_44C070 -> entry +137..+155): no guilds / awakening here yet (0).
    uint8_t  tutorial_step{ 0 };
    uint8_t  guild_state{ 0 };
    uint8_t  is_guild_chief{ 0 };
    uint8_t  hack_index{ 0 };         // char_hack_info.hack_index
    uint8_t  awakening_flag{ 0 };     // tbl_char_awakening.awakening_flag
    uint32_t couple_code{ 0 };        // couple_char.couple_code
    int16_t  tm_level{ 0 };           // Paula's "tm_level for korea client" (+154)
    // The equipped items' looks by equip position (item_where 0 / item_pos -> entry +45 + 4*pos):
    // the item id, or the mixed item of its option 12 when it has one.
    uint32_t equip[23]{};
};
