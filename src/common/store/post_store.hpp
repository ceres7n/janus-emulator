#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "database.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  PostStore — the post box's procedures (the Paula DB's USPM_*Post* / *Gift* + USP_PostDate_Update,
//  called by the retail MasterServer's cPostStore), on the post / post_date / post_item tables
//  (sql/013_postbox.sql) and the item rows of char_inventory / char_item_option.
//  Every method returns 0 or the procedure's error number (101..) — the client shows those itself
//  (S2C 1232 / 1235 / 1238 / 1241 / 1244 [u32 code], client sub_B5C800: >= 101 -> sub_B5BC80).
//  Times are local "YYYY-MM-DD HH:MM:SS" strings (the procedures' GETDATE()).
// ─────────────────────────────────────────────────────────────────────────────

// One post as a list shows it: the char's latest post_date row of the post, the other side's name.
struct PostRow {
    uint64_t    date_seq{ 0 };
    uint32_t    post_uid{ 0 };
    uint8_t     post_type{ 0 };          // 0 the post box, 1 the gift box
    uint8_t     sub_type{ 0 };           // 0 normal, 1 gift, 2 trade (gelders requested), 3 payment, 4 paid gift, 5 GM gift
    uint8_t     state{ 0 };              // 0 unread, 1 read, 2 returned
    uint8_t     user_type{ 0 };          // 0 we sent it, 1 we got it
    std::string char_name;               // the other side
    std::string title;
    std::string occur_date;              // its row's SendDate
    std::string return_date;
};

// One attached item: the post_item row (+ a rare item's char_inventory row).
struct PostItemRow {
    uint64_t item_seq{ 0 };
    uint64_t item_uid{ 0 };
    uint8_t  item_class{ 1 };
    uint32_t item_index{ 0 };
    int16_t  item_attr{ 0 };             // the count
    int32_t  add_minute{ 0 };
    uint8_t  item_flag{ 0 };
    uint8_t  refine{ 0 }, refine_state{ 0 }, item_slot{ 0 };   // a rare item's own row
};

// One option of an attached rare item (char_item_option).
struct PostOptionRow {
    uint64_t item_seq{ 0 };
    uint64_t item_uid{ 0 };
    uint8_t  type{ 10 };
    int16_t  index{ 0 };
    int16_t  value{ 0 };
    int32_t  mixed{ 0 };
};

// One item change of the stream lists ((ItemUID, ItemIndex, ItemClass, LastCount, UpdateFlag): flag 0 the
// item leaves the bag, 1 a new place / row, 2 a stack's count becomes LastCount).
struct PostItemOp {
    uint64_t uid{ 0 };
    uint32_t index{ 0 };
    uint8_t  cls{ 1 };
    uint16_t count{ 0 };
    uint8_t  flag{ 0 };
    uint8_t  slot{ 0 };                  // (receive) a new common row's Item_Slot
    uint64_t item_seq{ 0 };              // (receive) the post_item it comes from
    uint64_t new_uid{ 0 };               // (receive) the row it became
};

class PostStore {
public:
    explicit PostStore(Database& db) : db_(db) {}

    static std::string Now();                                       // GETDATE()
    static std::string DateInfo(const std::string& from, int type); // FN_PostDateInfo: 0 2050, 1 +7 d, 2 +1 d, else +3 d

    // USP_PostDate_Update: the account's posts past their ReturnDate move on (unread -> returned to the
    // sender for 7 more days, read / returned -> deleted; a payment -> deleted).
    int Expire(uint32_t user_uid);
    // USPM_Get_PostCount: the unread (post box: this char's; gift box: the account's) and returned posts.
    int Count(uint32_t char_uid, int& fresh, int& returned);
    // USPM_Get_PostList: 0 received, 1 sent, 2 the gift box; the rows older than date_seq (0: the newest).
    int List(uint8_t list_type, uint32_t user_uid, uint32_t char_uid, uint64_t date_seq, int rows,
             std::vector<PostRow>& out);
    // USPM_Get_PostSync: the counts, then the post box's and the gift box's rows newer than date_seq.
    int Sync(uint32_t user_uid, uint32_t char_uid, uint64_t date_seq, int& fresh, int& returned,
             std::vector<PostRow>& box, std::vector<PostRow>& gifts);
    // USPM_Get_PostDetailInfo (post box): its content, gelders, items + options; an unread / returned
    // normal post we got becomes read.
    int Detail(uint32_t post_uid, uint32_t user_uid, uint32_t char_uid, std::string& content, int32_t& money,
               std::vector<PostItemRow>& items, std::vector<PostOptionRow>& opts);
    // USPM_Get_GiftDetailInfo (gift box): its content and items; one without items becomes read.
    int GiftDetail(uint32_t post_uid, uint32_t user_uid, uint32_t char_uid, std::string& content,
                   std::vector<PostItemRow>& items);

    struct SendArgs {
        uint8_t     sub_type{ 0 };
        std::string title, content;
        int32_t     tax{ 0 }, money{ 0 };
        uint32_t    from_user{ 0 }, from_char{ 0 };
        std::string from_name;
        int32_t     from_level{ 0 };
        std::string to_name;
        int32_t     from_money_final{ -1 };  // the sender's gelders after the gift's money + fee (< 0: unchanged)
    };
    // USPM_Send_PostItem: `from` the sender's bag changes (0 leaves / 2 count), `to` the post's items (1).
    int Send(const SendArgs& a, const std::vector<PostItemOp>& from, const std::vector<PostItemOp>& to,
             uint32_t& post_uid);

    struct RecvArgs {
        uint32_t    post_uid{ 0 };
        uint8_t     state{ 0 };
        uint32_t    to_user{ 0 }, to_char{ 0 };
        int16_t     to_level{ 0 };
        std::string to_name, sys_title, sys_content;
        int32_t     tax{ 0 }, money{ 0 };
        int32_t     to_money_final{ -1 };    // the receiver's gelders afterwards (< 0: unchanged)
    };
    // USPM_Recv_PostItem: the items into the bag (flag 1 new row / rare item's owner, 2 joins a stack),
    // the gelders, a trade post's payment post to its sender; the post becomes read.
    int Recv(const RecvArgs& a, std::vector<PostItemOp>& ops);
    // USPM_Recv_GiftItem: every item a new row (a rare one with its options); the gift becomes read.
    int RecvGift(const RecvArgs& a, std::vector<PostItemOp>& ops, const std::vector<PostOptionRow>& opts);
    // USPM_Delete_Post: unread mail we got goes back (returned), read mail / our sent mail is deleted;
    // `next` = the next older row of that list (to fill the page).
    int Delete(uint32_t post_uid, uint32_t user_uid, uint32_t char_uid, uint64_t del_seq, uint64_t last_seq,
               std::vector<PostRow>& next);

    struct GiftItem { uint32_t index{ 0 }; uint8_t cls{ 1 }; int16_t attr{ 1 }; int32_t add_minute{ 0 }; };
    // USPM_Send_GMGiftItem: a GM gift (gift box, sub type 5, kept until 2050) from `gm_name`.
    int SendGMGift(const std::string& title, const std::string& content, uint32_t to_char,
                   const std::string& gm_name, const std::vector<GiftItem>& items);

private:
    Database& db_;
};
