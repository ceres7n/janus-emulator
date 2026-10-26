#include "store/char_store.hpp"
#include "logger.hpp"
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

// A character's char-select columns (the procedures read view_char_attr_status_state_equip_all +
// char_hack_info + tbl_couple_char; guilds and awakening are not there yet).
constexpr const char* kCharCols =
    "c.uid, c.owner, c.slot, c.name, c.type, c.job, c.f_type, c.hair, c.hair_2, c.hair_3, c.job_state, "
    "c.pts_p, c.pts_m, c.pts_s, c.pts_c, c.type_2, c.job_2, c.type_3, c.job_3, c.level, "
    "c.zone, c.pos_x, c.pos_y, c.home, c.money, c.state, c.authority, c.deleted, c.freeze_until, "
    "c.tutorial_step, c.tm_level, COALESCE(h.hack_index, 0) AS hack_index, "
    "COALESCE(cc.couple_code, 0) AS couple_code";
constexpr const char* kCharFrom =
    "FROM characters c LEFT JOIN char_hack_info h ON h.char_uid = c.uid "
    "LEFT JOIN couple_char cc ON cc.char_uid = c.uid";

CharRecord char_row(const DBRow& r)
{
    CharRecord c{};
    c.uid       = std::stoul(r.at("uid"));
    c.owner     = std::stoul(r.at("owner"));
    c.slot      = static_cast<uint8_t>(std::stoi(r.at("slot")));
    std::strncpy(c.name, r.at("name").c_str(), 17);
    c.type      = static_cast<int16_t>(std::stoi(r.at("type")));
    c.job       = static_cast<int16_t>(std::stoi(r.at("job")));
    c.f_type    = static_cast<uint8_t>(std::stoi(r.at("f_type")));
    c.hair      = static_cast<uint8_t>(std::stoi(r.at("hair")));
    c.hair_2    = static_cast<uint8_t>(std::stoi(r.at("hair_2")));
    c.hair_3    = static_cast<uint8_t>(std::stoi(r.at("hair_3")));
    c.job_state = static_cast<uint8_t>(std::stoi(r.at("job_state")));
    c.pts_p     = static_cast<uint8_t>(std::stoi(r.at("pts_p")));
    c.pts_m     = static_cast<uint8_t>(std::stoi(r.at("pts_m")));
    c.pts_s     = static_cast<uint8_t>(std::stoi(r.at("pts_s")));
    c.pts_c     = static_cast<uint8_t>(std::stoi(r.at("pts_c")));
    c.type_2    = static_cast<int16_t>(std::stoi(r.at("type_2")));
    c.job_2     = static_cast<uint8_t>(std::stoi(r.at("job_2")));
    c.type_3    = static_cast<int16_t>(std::stoi(r.at("type_3")));
    c.job_3     = static_cast<uint8_t>(std::stoi(r.at("job_3")));
    c.level     = static_cast<int16_t>(std::stoi(r.at("level")));
    c.zone      = static_cast<int16_t>(std::stoi(r.at("zone")));
    c.pos_x     = std::stoi(r.at("pos_x"));
    c.pos_y     = std::stoi(r.at("pos_y"));
    c.home      = static_cast<int16_t>(std::stoi(r.at("home")));
    c.money     = std::stoi(r.at("money"));
    c.state     = static_cast<uint8_t>(std::stoi(r.at("state")));
    c.authority = static_cast<uint8_t>(std::stoi(r.at("authority")));
    c.deleted   = static_cast<uint8_t>(std::stoi(r.at("deleted")));
    std::strncpy(c.freeze_until, r.at("freeze_until").c_str(), 19);
    c.tutorial_step = static_cast<uint8_t>(std::stoi(r.at("tutorial_step")));
    c.tm_level      = static_cast<int16_t>(std::stoi(r.at("tm_level")));
    c.hack_index    = static_cast<uint8_t>(std::stoi(r.at("hack_index")));
    c.couple_code   = static_cast<uint32_t>(std::stoul(r.at("couple_code")));
    return c;
}

// An equipped item's look (the procedures' second result: tbl_item_char item_where 0 / item_pos +
// tbl_item + the item's option 12): the mixed item of the option when it has one, else the item id.
// GetByOwnerFromDb / InsertIntoDb keep positions < 23.
void put_look(CharRecord& c, const DBRow& r, uint32_t owner)
{
    const int pos = std::stoi(r.at("equip_slot"));
    if (pos < 0 || pos >= 23) {
        LOG_ERROR("cUserStore::GetByOwnerFromDb : GetItem : owner:%u, slot:%u, pos:%d", owner, c.slot, pos);
        return;
    }
    const int mixed = std::stoi(r.at("mixed_item"));
    c.equip[pos] = mixed ? static_cast<uint32_t>(mixed) : static_cast<uint32_t>(std::stoul(r.at("item_id")));
}

constexpr const char* kLookCols =
    "c.slot, i.item_id, i.equip_slot, COALESCE(o.mixed_item, 0) AS mixed_item "
    "FROM char_inventory i JOIN characters c ON c.uid = i.char_uid "
    "LEFT JOIN char_item_option o ON o.item_uid = i.uid AND o.option_type = 12";

} // namespace

std::vector<CharRecord> CharStore::LoadCharsByOwner(uint32_t owner, uint8_t& usable_slot)
{
    // SELECT @slot = usable_slot FROM user_info (no row: NULL, and `slot < NULL` lists nothing)
    usable_slot = 0;
    auto acc = db_.query("SELECT usable_slot FROM accounts WHERE account_id=%u", owner);
    if (!acc.empty()) usable_slot = static_cast<uint8_t>(std::stoi(acc[0].at("usable_slot")));

    // The Paula fixes of the Animal Lover line's job changes ("AF: new logic here to fix second job
    // advancement"): a Bear whose Trainer (18) landed in the 3rd-tier columns gets it back as its 2nd
    // job (shown as tier 2, not 3); a Zoologist (27 / 36) without the 2nd tier gets the Trainer; then
    // every character of the server whose last tier is the Trainer and shows tier 3 shows tier 2.
    db_.execute("UPDATE characters SET type_2=18, job_2=18, type_3=0, job_3=0, "
                "job_state=IF(job_state=3, 2, job_state) "
                "WHERE owner=%u AND type=9 AND type_3=18 AND job_3=18", owner);
    db_.execute("UPDATE characters SET type_2=18, job_2=18 "
                "WHERE owner=%u AND type=9 AND type_2=0 AND job_2=0 AND type_3=27 AND job_3=36", owner);
    db_.execute("UPDATE characters SET job_state=2 "
                "WHERE type_2=18 AND job_2=18 AND type_3=0 AND job_3=0 AND job_state=3");
    // (the procedure then gives the account's deleted characters authority 2 — ours keep deleted=1 —
    // and runs login events dated 2012; tbl_sleep_char: no hibernation here)

    std::vector<CharRecord> out;
    for (auto& r : db_.query("SELECT %s %s WHERE c.owner=%u AND c.deleted=0 AND c.authority<>2 AND c.slot<%u "
                             "ORDER BY c.slot", kCharCols, kCharFrom, owner, static_cast<unsigned>(usable_slot)))
        out.push_back(char_row(r));

    // The worn items, of the characters in the first 3 slots only (Paula: "master pointer limit").
    for (auto& r : db_.query("SELECT %s WHERE c.owner=%u AND c.deleted=0 AND c.authority<>2 AND i.item_where=1 "
                             "AND i.equip_slot>=0 AND c.slot<3 ORDER BY c.slot", kLookCols, owner)) {
        const int slot = std::stoi(r.at("slot"));
        CharRecord* c = nullptr;
        for (CharRecord& x : out) if (x.slot == slot) { c = &x; break; }
        if (!c) {
            LOG_ERROR("cUserStore::GetByOwnerFromDb : GetItem : owner:%u, slot:%d, pos:%s", owner, slot,
                      r.at("equip_slot").c_str());
            continue;
        }
        put_look(*c, r, owner);
    }
    return out;
}

bool CharStore::NameExists(const std::string& name)
{
    auto rows = db_.query("SELECT uid FROM characters WHERE name = '%s' AND deleted=0",
                          db_.escape(name).c_str());
    return !rows.empty();
}

namespace {

// cCharStore::InsertIntoDb's first check (sub_454B60): the 4 growth points sum to 10, each is 1..4,
// and the one of the character's f_type is 4.
bool levelup_pts_ok(const CharRecord& c)
{
    if (c.pts_p + c.pts_m + c.pts_s + c.pts_c != 10) return false;
    for (uint8_t v : { c.pts_p, c.pts_m, c.pts_s, c.pts_c })
        if (v == 0 || v > 4) return false;
    if (c.f_type == 0 && c.pts_p != 4) return false;
    if (c.f_type == 1 && c.pts_m != 4) return false;
    if (c.f_type == 2 && c.pts_s != 4) return false;
    return c.f_type != 3 || c.pts_c == 4;
}

// The name as fn_CharacterName_ValidCheck sees it. The Paula database is Thai_CI_AS (code page 874;
// tbl_forbidden holds Thai words) while the KR client sends CP949: a character that code page lacks
// reaches the varchar as '?' — hence tbl_forbidden's '?', '??' and 'G?' rows, and every one of the
// 16,046 Paula names is ASCII. A byte >= 0x80 and the byte after it (the client's / LoginServer's
// double-byte pairing) are one '?'.
std::string db_view(const std::string& name)
{
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(name[i]);
        if (b < 0x80) { out += static_cast<char>(b); continue; }
        out += '?';
        if (i + 1 < name.size()) ++i;
    }
    return out;
}

char fold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// SQL Server `s LIKE pat ESCAPE '\'` under a case-insensitive collation: '%' any run (none too),
// '_' one character, '\' takes the next one as itself; letters compare without case. The subject is
// ASCII here (db_view), so a byte is a character.
bool like_ci(const std::string& s, const std::string& pat)
{
    size_t si = 0, pi = 0, star = std::string::npos, mark = 0;
    while (si < s.size()) {
        if (pi < pat.size()) {
            const char pc = pat[pi];
            if (pc == '%') { star = ++pi; mark = si; continue; }
            if (pc == '_') { ++pi; ++si; continue; }
            if (pc == '\\') {
                if (pi + 1 < pat.size() && fold(s[si]) == fold(pat[pi + 1])) { pi += 2; ++si; continue; }
            } else if (fold(s[si]) == fold(pc)) { ++pi; ++si; continue; }
        }
        if (star == std::string::npos) return false;
        pi = star;
        si = ++mark;
    }
    while (pi < pat.size() && pat[pi] == '%') ++pi;
    return pi == pat.size();
}

// `a = b` under the same collation: no case, trailing spaces ignored.
bool equal_ci(std::string a, std::string b)
{
    while (!a.empty() && a.back() == ' ') a.pop_back();
    while (!b.empty() && b.back() == ' ') b.pop_back();
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (fold(a[i]) != fold(b[i])) return false;
    return true;
}

// fn_CharacterName_ValidCheck (Paula): no tbl_forbidden type-2 string anywhere in the name (LIKE
// '%' + string + '%' ESCAPE '\'), no type-1 string as the whole name, and every character up to
// LEN() (trailing spaces left out) an ASCII letter, digit, '^' or '_' (LIKE '%[^a-z^0-9^_]%': the
// '^' after the first is a plain character) — or, past ASCII, not a full-width form U+FF01..U+FF5E
// (none get here: db_view made them '?').
bool name_valid(Database& db, const std::string& raw)
{
    const std::string name = db_view(raw);
    auto forbidden = db.query("SELECT forbidden_type, forbidden_string FROM forbidden ORDER BY id");
    for (auto& f : forbidden)
        if (f.at("forbidden_type") == "2" && like_ci(name, "%" + f.at("forbidden_string") + "%")) return false;
    for (auto& f : forbidden)
        if (f.at("forbidden_type") == "1" && equal_ci(name, f.at("forbidden_string"))) return false;
    size_t len = name.size();
    while (len > 0 && name[len - 1] == ' ') --len;
    for (size_t i = 0; i < len; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '^' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// usp_user_set_mycamp_item: the account's MyCamp starter set, once per account (tbl_user_mycamp_log) —
// every give_type 1 row, then every row of 3 random give_type 2 indexes (the join takes all rows of
// an index, whatever their give_type). 0, or 103 / 102 / 101 when a write fails.
int set_mycamp_item(Database& db, uint32_t user)
{
    if (!db.query("SELECT user_uid FROM user_mycamp_log WHERE user_uid=%u", user).empty()) return 0;
    try {
        db.execute("INSERT INTO mycamp_item (user_uid, item_index, item_where, item_buy_date, item_use_date, "
                   "item_use_time, item_flag, sell_code, sell_type) "
                   "SELECT %u, item_index, 0, NOW(), NULL, 0, 0, 200000001, 0 FROM user_mycamp_item "
                   "WHERE give_type=1 ORDER BY id", user);
    } catch (const DBException&) { return 103; }
    try {
        db.execute("INSERT INTO mycamp_item (user_uid, item_index, item_where, item_buy_date, item_use_date, "
                   "item_use_time, item_flag, sell_code, sell_type) "
                   "SELECT %u, ii.item_index, 0, NOW(), NULL, 0, 0, 200000001, 0 "
                   "FROM (SELECT item_index FROM user_mycamp_item WHERE give_type=2 GROUP BY item_index "
                   "      ORDER BY RAND() LIMIT 3) AS i "
                   "JOIN user_mycamp_item AS ii ON ii.item_index = i.item_index ORDER BY ii.id", user);
    } catch (const DBException&) { return 102; }
    try {
        db.execute("INSERT INTO user_mycamp_log (user_uid, created_date) VALUES (%u, NOW())", user);
    } catch (const DBException&) { return 101; }
    return 0;
}

// usp_user_set_mycamp_item_event (the 2008.06.26 sign-up event): an account made from 2008-06-25 on
// gets 5 random rows of tbl_user_mycamp_item_event (each with every row of its index), once
// (tbl_user_mycamp_log_event). The Paula table is empty: only the log row is written.
void set_mycamp_item_event(Database& db, uint32_t user)
{
    auto acc = db.query("SELECT created_at >= '2008-06-25' AS ok FROM accounts WHERE account_id=%u", user);
    if (acc.empty() || acc[0].at("ok") != "1") return;          // (no row: '2007-01-01')
    if (!db.query("SELECT user_uid FROM user_mycamp_log_event WHERE user_uid=%u", user).empty()) return;
    db.execute("INSERT INTO mycamp_item (user_uid, item_index, item_where, item_buy_date, item_use_date, "
               "item_use_time, item_flag, sell_code, sell_type) "
               "SELECT %u, ii.item_index, 0, NOW(), NULL, 0, 0, 200000001, 0 "
               "FROM (SELECT item_index FROM user_mycamp_item_event ORDER BY RAND() LIMIT 5) AS i "
               "JOIN user_mycamp_item_event AS ii ON ii.item_index = i.item_index", user);
    db.execute("INSERT INTO user_mycamp_log_event (user_uid, created_date) VALUES (%u, NOW())", user);
}

} // namespace

int CharStore::MakeNewCharacter(CharRecord& chr)
{
    if (!levelup_pts_ok(chr)) {
        LOG_ERROR("cCharStore::InsertIntoDb - invalid levelup pts:%d,%d,%d,%d", chr.pts_p, chr.pts_c, chr.pts_m,
                  chr.pts_s);
        return 9999;
    }
    const std::string name(chr.name);

    // ── usp_insert_character ──
    // The slots: at most 12 characters, and no more than the account's usable_slot.
    const int slotcount = std::stoi(db_.query(
        "SELECT COUNT(*) AS n FROM characters WHERE owner=%u AND deleted=0 AND authority<>2", chr.owner)[0].at("n"));
    if (slotcount > 11) {
        LOG_WARN("usp_insert_character (1020)Overflow slot count!! : owner %u", chr.owner);
        return 1020;
    }
    auto acc = db_.query("SELECT usable_slot FROM accounts WHERE account_id=%u", chr.owner);
    if (!acc.empty() && slotcount > std::stoi(acc[0].at("usable_slot")) - 1) {
        LOG_WARN("usp_insert_character (1020)Overflow slot count!! : owner %u", chr.owner);
        return 1020;
    }
    // A taken slot: the procedure sets 1021 'Already exist slot number!!' but goes on (no goto, and its
    // end returns 0) — the character is made all the same (the LoginServer answers 1065).
    if (!db_.query("SELECT uid FROM characters WHERE owner=%u AND deleted=0 AND authority<>2 AND slot=%u",
                   chr.owner, static_cast<unsigned>(chr.slot)).empty())
        LOG_WARN("usp_insert_character: owner %u slot %u already used (1021, not stopped)", chr.owner, chr.slot);
    if (NameExists(name)) {
        LOG_WARN("cCharStore::InsertIntoDb failure: spRet:1023 (name '%s')", name.c_str());
        return 1023;
    }
    // (tbl_sleep_char, the hibernated characters' names: no hibernation here)
    if (!name_valid(db_, name)) {
        LOG_WARN("cCharStore::InsertIntoDb failure: spRet:1019 (name '%s')", name.c_str());
        return 1019;
    }

    DBTransaction tran(db_);
    unsigned long long uid = 0;
    try {
        // char_attr + char_status (본섭용: level 1, nothing else) + char_state: the zone and the return
        // town of the request at (0,0) with 0 HP / MP (the GameServer places it and gives it 30% HP,
        // Zone::PrepareToStartPlayer / SetPlayerFromLoginInfo), job_state 1, 80 warehouse slots.
        uid = db_.insert(
            "INSERT INTO characters (owner, slot, name, type, job, f_type, pts_p, pts_m, pts_s, pts_c, created_at, "
            "  type_2, job_2, job_2_changed, transgender_date, type_3, job_3, job_3_changed, change_job_queststate, "
            "  ap_pts, ac_pts, dx_pts, max_mp_pts, ma_pts, md_pts, max_wt_pts, da_pts, lk_pts, max_hp_pts, dp_pts, "
            "  hv_pts, attr_pts, tm_level_p, tm_level_m, tm_level_s, tm_level_c, level, tm_point, tm_level, tm_exp, "
            "  tutorial_step, state, authority, zone, pos_x, pos_y, home, money, hp, mp, "
            "  tm_exp_p, tm_exp_m, tm_exp_s, tm_exp_c, exp, last_login, hair, hair_2, job_state, wh_slots, hair_3) "
            "VALUES (%u, %u, '%s', %d, %d, %u, %u, %u, %u, %u, NOW(), "
            "  0, 0, NOW(), '2000-01-01', 0, 0, NOW(), 0, "
            "  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
            "  0, 0, 0, 0, 0, 0, 1, 0, 0, 0, "
            "  0, 0, 0, %d, 0, 0, %d, 0, 0, 0, "
            "  0, 0, 0, 0, 0, NOW(), %u, 0, 1, 80, 0)",
            chr.owner, static_cast<unsigned>(chr.slot), db_.escape(name).c_str(), chr.type, chr.job,
            static_cast<unsigned>(chr.f_type), static_cast<unsigned>(chr.pts_p), static_cast<unsigned>(chr.pts_m),
            static_cast<unsigned>(chr.pts_s), static_cast<unsigned>(chr.pts_c), chr.zone, chr.home,
            static_cast<unsigned>(chr.hair));
    } catch (const DBException&) { uid = 0; }
    if (!uid) {
        LOG_ERROR("usp_insert_character (9001)char_attr Insert Fail!! : owner %u, name '%s'", chr.owner, name.c_str());
        return 9001;
    }

    try {
        // The items (tbl_item + tbl_item_char -> char_inventory, in the bag in this order).
        int bag = 100;
        auto give = [&](uint32_t item, unsigned amount, int equip_slot) {
            db_.execute("INSERT INTO char_inventory (char_uid, item_id, amount, bag_slot, equip_slot, item_where) "
                        "VALUES (%llu, %u, %u, %d, %d, 1)", uid, item, amount, bag++, equip_slot);
        };
        // 귀, 꼬리 (착용): the race's ears and tail, worn (item_pos 5 / 6). (Another type has none: the
        // procedure's CASE gives NULL and its unchecked inserts write nothing usable.)
        static const uint32_t kEars[10] = { 0, 6900, 6901, 6902, 6903, 6904, 6905, 6906, 6907, 6932 };
        static const uint32_t kTail[10] = { 0, 6950, 6951, 6952, 6953, 6954, 6955, 6956, 6957, 6982 };
        if (chr.type >= 1 && chr.type <= 9) {
            give(kEars[chr.type], 1, 5);
            give(kTail[chr.type], 1, 6);
        }
        // 복포션 (2008.06.26 sign-up event: 30 of each): the Lucky Pink / Blue Potions.
        give(2006, 30, -1);
        give(2206, 30, -1);
        // Duck set: the Duckling Staff (the Bear: the Duckling Hammer), Hat, Shield and Keychain.
        give(chr.type == 9 ? 75920 : 17310, 1, -1);
        give(17910, 1, -1);
        give(18810, 1, -1);
        give(19241, 1, -1);
        // polar hammer (the Bear): the Beginner Hammer, Puchi Bear, Neo hammer of Hunting, Strong Ring.
        if (chr.type == 9) {
            give(92018, 1, -1);
            give(90163, 1, -1);
            give(40178, 1, -1);
            give(7284, 1, -1);
        }
        // (the drill 8000 and the f_type skill cards 27012 / 28014 / 29015 / 30013 are commented out)
    } catch (const DBException&) {
        LOG_ERROR("usp_insert_character: the starter items failed (owner %u)", chr.owner);
        return 9996;
    }

    // 2010.07.28: the f_type's skill given learned (the Bear: 4701 + 4709) — another f_type has none
    // and its NULL insert fails.
    try {
        if (chr.type == 9) {
            db_.execute("INSERT INTO char_skills (char_uid, skill_id, level) VALUES (%llu, 4701, 1)", uid);
            db_.execute("INSERT INTO char_skills (char_uid, skill_id, level) VALUES (%llu, 4709, 1)", uid);
        } else {
            static const int kSkill[4] = { 1012, 2014, 3015, 4013 };
            if (chr.f_type > 3) throw DBException("skill_id NULL");
            db_.execute("INSERT INTO char_skills (char_uid, skill_id, level) VALUES (%llu, %d, 1)", uid,
                        kSkill[chr.f_type]);
        }
    } catch (const DBException&) {
        LOG_ERROR("usp_insert_character (105)tbl_skill insert fail : owner %u", chr.owner);
        return 105;
    }

    // The MyCamp slot 'mycamp 1' with one cell (18001) and two walls (18002) on it (마이캠프 슬롯 생성시
    // 셀1개, 벽2개 생성되어야 한다), then the account's MyCamp sets.
    try {
        const unsigned long long slot_code =
            db_.insert("INSERT INTO mycamp_slot (char_uid, slot_name, slot_uptodate) VALUES (%llu, 'mycamp 1', NOW())",
                       uid);
        auto camp_item = [&](int index) {
            return db_.insert("INSERT INTO mycamp_item (user_uid, item_index, item_where, item_buy_date, "
                              "item_use_date, item_use_time, item_flag, sell_code, sell_type) "
                              "VALUES (%u, %d, 1, NOW(), NOW(), 0, 0, 0, 0)", chr.owner, index);
        };
        const unsigned long long cell = camp_item(18001), wall1 = camp_item(18002), wall2 = camp_item(18002);
        if (!slot_code || !cell || !wall1 || !wall2) throw DBException("mycamp");
        db_.execute("INSERT INTO mycamp_deco (slot_code, item_uid, deco_x, deco_y, deco_z, deco_direction, parents_uid) "
                    "VALUES (%llu, %llu, 2400, 2400, 0, 0, %llu), (%llu, %llu, 0, -120, 0, 0, %llu), "
                    "(%llu, %llu, 0, -120, 0, 1, %llu)",
                    slot_code, cell, cell, slot_code, wall1, cell, slot_code, wall2, cell);
    } catch (const DBException&) {
        LOG_ERROR("usp_insert_character (105)tbl_mycamp_slot insert fail : owner %u", chr.owner);
        return 105;
    }
    if (const int r = set_mycamp_item(db_, chr.owner)) {
        LOG_ERROR("usp_user_set_mycamp_item(%d): user_uid = %u", r, chr.owner);
        return r;
    }
    // (the procedure tests @r again instead of the event's @r2: its failure stops nothing)
    try { set_mycamp_item_event(db_, chr.owner); }
    catch (const DBException&) { LOG_ERROR("usp_user_set_mycamp_item_event: user_uid = %u", chr.owner); }

    // 발굴 기록할 로우 등록 (the drill ranking row) and 커플시스템 관련 테이블 생성 (the couple row).
    try {
        db_.execute("INSERT INTO drill_digup (uid, depth_count, dig_count, regdate) VALUES (%llu, 0, 0, NOW())", uid);
    } catch (const DBException&) {
        LOG_ERROR("usp_insert_character (105)tbl_drill_digup insert fail : owner %u", chr.owner);
        return 105;
    }
    try {
        db_.execute("INSERT INTO couple_char (char_uid, couple_code, couple_count, couple_delete_count, "
                    "marriage_count, divorce_count, wedding_in_count, create_date) "
                    "VALUES (%llu, 0, 0, 0, 0, 0, 0, NOW())", uid);
    } catch (const DBException&) {
        LOG_ERROR("usp_insert_character (105)tbl_couple_char insert fail : owner %u", chr.owner);
        return 105;
    }
    tran.commit();

    // The procedure's two results: the character and its worn items (InsertIntoDb keeps pos < 23).
    auto rows = db_.query("SELECT %s %s WHERE c.uid=%llu", kCharCols, kCharFrom, uid);
    if (rows.empty()) return 9996;
    chr = char_row(rows[0]);
    for (auto& r : db_.query("SELECT %s WHERE i.char_uid=%llu AND i.item_where=1 AND i.equip_slot>=0",
                             kLookCols, uid))
        put_look(chr, r, chr.owner);
    return 0;
}

int CharStore::DeleteFromDb(uint32_t charUid, uint32_t owner, const std::string& user_ip)
{
    // (the LoginServer only asks for a character of its own list: the owner check stands for that)
    auto rows = db_.query("SELECT name FROM characters WHERE uid=%u AND owner=%u AND deleted=0", charUid, owner);
    if (rows.empty()) return 8001;
    const std::string old_name = rows[0].at("name");
    // (a guild member, 8004, or a married character, 8005, cannot be deleted: no guilds / couples here yet)

    DBTransaction tran(db_);
    // The name becomes '0' x (16 - len) + uid + '+' (17 characters), which frees the old one, and the
    // character gets authority 2 (ours also keeps deleted=1).
    std::string uid_text = std::to_string(charUid);
    const std::string deleted_name = std::string(16 - uid_text.size(), '0') + uid_text + "+";
    try {
        if (db_.execute("UPDATE characters SET name='%s', authority=2, deleted=1 WHERE uid=%u", deleted_name.c_str(),
                        charUid) != 1)
            return 8003;
    } catch (const DBException&) { return 8003; }
    try {
        db_.execute("INSERT INTO deleted_char (user_uid, char_uid, char_name, user_ip, deleted_date) "
                    "VALUES (%u, %u, '%s', '%s', NOW())", owner, charUid, db_.escape(old_name).c_str(),
                    db_.escape(user_ip.substr(0, 15)).c_str());
    } catch (const DBException&) { return 8002; }

    // The tables it leaves (friend_info / friend_sms: the friends both ways); the MyCamp cell and walls
    // (18001 / 18002 never sold) placed in its slots and their decorations — the slots stay.
    db_.execute("DELETE FROM friend_info WHERE ownerid=%u", charUid);
    db_.execute("DELETE FROM friend_info WHERE friendid=%u", charUid);
    db_.execute("DELETE FROM friend_sms WHERE sender=%u", charUid);
    std::vector<std::string> cell_wall;
    for (auto& r : db_.query("SELECT item_uid FROM mycamp_item WHERE user_uid=%u AND item_index IN (18001, 18002) "
                             "AND sell_code=0 AND item_uid IN (SELECT d.item_uid FROM mycamp_deco d "
                             "JOIN mycamp_slot s ON s.slot_code = d.slot_code WHERE s.char_uid=%u)", owner, charUid))
        cell_wall.push_back(r.at("item_uid"));
    db_.execute("DELETE FROM mycamp_deco WHERE slot_code IN (SELECT slot_code FROM mycamp_slot WHERE char_uid=%u)",
                charUid);
    for (const std::string& item : cell_wall)
        db_.execute("DELETE FROM mycamp_item WHERE user_uid=%u AND item_uid=%s", owner, item.c_str());
    // (the guild application, tbl_guild_member_standby, and the unique items, uspm_unique_item_char_all_deprive:
    // not here yet)
    tran.commit();
    return 0;
}

bool CharStore::SaveState(uint32_t uid, int16_t zone, int32_t pos_x, int32_t pos_y,
                          int32_t money, int16_t level, int32_t exp, int32_t hp, int32_t mp)
{
    uint64_t affected = db_.execute(
        "UPDATE characters SET zone=%d, pos_x=%d, pos_y=%d, money=%d, "
        "level=%d, exp=%d, hp=%d, mp=%d WHERE uid=%u AND deleted=0",
        zone, pos_x, pos_y, money, level, exp, hp, mp, uid
    );
    return affected > 0;
}

// The return town (retail Player +1656, persisted through the char state info, GetChrStateInfo
// sub_75CC70 +12 / SetChrStateInfo sub_75D1F0): stored in characters.home (default 1).
bool CharStore::SaveHome(uint32_t uid, int16_t home)
{
    uint64_t affected = db_.execute(
        "UPDATE characters SET home=%d WHERE uid=%u AND deleted=0", home, uid);
    return affected > 0;
}

bool CharStore::SaveStatPoints(uint32_t uid, const int32_t accpts[12], int32_t attr_pts, int32_t tm_level,
                               int32_t tm_exp, int32_t tm_point)
{
    uint64_t affected = db_.execute(
        "UPDATE characters SET ap_pts=%d, ac_pts=%d, dx_pts=%d, max_mp_pts=%d, ma_pts=%d, md_pts=%d, "
        "max_wt_pts=%d, da_pts=%d, lk_pts=%d, max_hp_pts=%d, dp_pts=%d, hv_pts=%d, "
        "attr_pts=%d, tm_level=%d, tm_exp=%d, tm_point=%d WHERE uid=%u AND deleted=0",
        accpts[0], accpts[1], accpts[2], accpts[3], accpts[4], accpts[5],
        accpts[6], accpts[7], accpts[8], accpts[9], accpts[10], accpts[11],
        attr_pts, tm_level, tm_exp, tm_point, uid
    );
    return affected > 0;
}

int CharStore::RenameCharacter(uint32_t uid, const std::string& new_name, uint32_t item_index, std::string& old_name)
{
    if (NameExists(new_name)) return 1023;                   // (tbl_sleep_char: no hibernation here)
    if (!name_valid(db_, new_name)) return 1019;
    try {
        DBTransaction tran(db_);
        auto cur = db_.query("SELECT name FROM characters WHERE uid=%u AND deleted=0", uid);
        if (cur.empty()) return 101;
        old_name = cur[0].at("name");
        if (db_.execute("UPDATE characters SET name='%s' WHERE uid=%u AND deleted=0", db_.escape(new_name).c_str(),
                        uid) != 1)
            return 101;
        char why[48];
        std::snprintf(why, sizeof(why), "by item using(%u)", item_index);
        if (db_.execute("INSERT INTO char_names (char_uid, old_name, new_name, change_date, reason, first_name) "
                        "VALUES (%u, '%s', '%s', NOW(), '%s', '%s')", uid, db_.escape(old_name).c_str(),
                        db_.escape(new_name).c_str(), why, db_.escape(old_name).c_str()) != 1)
            return 102;
        tran.commit();
    } catch (const DBException& e) {
        LOG_ERROR("uspm_insert_char_names_changed %u: %s", uid, e.what());
        return 101;
    }
    return 0;
}
