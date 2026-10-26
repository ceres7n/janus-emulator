#include "store/post_store.hpp"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>

namespace {

int to_i(const DBRow& r, const char* k)
{
    auto it = r.find(k);
    return (it == r.end() || it->second.empty()) ? 0 : std::stoi(it->second);
}
unsigned long long to_u64(const DBRow& r, const char* k)
{
    auto it = r.find(k);
    return (it == r.end() || it->second.empty()) ? 0ULL : std::stoull(it->second);
}
std::string to_s(const DBRow& r, const char* k)
{
    auto it = r.find(k);
    return it == r.end() ? std::string() : it->second;
}

std::string fmt_tm(const std::tm& t)
{
    char b[24];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
    return b;
}

bool parse_tm(const std::string& s, std::tm& t)
{
    t = std::tm{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return false;
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = h; t.tm_min = mi; t.tm_sec = se;
    t.tm_isdst = -1;
    return true;
}

// The char's latest post_date row of each post (RANK() OVER (PARTITION BY PostUID ORDER BY DateSeq DESC) = 1)
// joined with the other side's row at that state (its CharName) — the shape every list query of the
// procedures has. `char_uid` 0 = the whole account (the gift box).
std::vector<PostRow> latest_rows(Database& db, uint32_t user_uid, uint32_t char_uid, uint8_t post_type,
                                 uint8_t my_type, uint8_t other_type, uint8_t max_state, const std::string& now,
                                 const std::string& extra, int limit)
{
    char who[48] = "";
    if (char_uid) std::snprintf(who, sizeof(who), " AND d.char_uid = %u", char_uid);
    char lim[32] = "";
    if (limit > 0) std::snprintf(lim, sizeof(lim), " LIMIT %d", limit);
    auto rows = db.query(
        "SELECT a.date_seq, a.post_uid, a.post_type, a.post_sub_type, a.post_state, a.user_type, a.title, "
        "c.char_name, a.send_date, a.return_date FROM ("
        " SELECT p.post_type, p.post_sub_type, p.title, d.date_seq, d.post_uid, d.post_state, d.user_type, "
        " d.send_date, d.return_date, RANK() OVER (PARTITION BY d.post_uid ORDER BY d.date_seq DESC) AS rn"
        " FROM post p JOIN post_date d ON p.post_uid = d.post_uid AND p.post_state < 3"
        " WHERE d.user_uid = %u%s) a "
        "JOIN post_date c ON a.post_uid = c.post_uid AND a.post_state = c.post_state AND c.user_type = %u "
        "WHERE a.rn = 1 AND a.post_state <= %u AND a.return_date > '%s' AND a.post_type = %u AND a.user_type = %u%s "
        "ORDER BY a.date_seq DESC%s",
        user_uid, who, static_cast<unsigned>(other_type), static_cast<unsigned>(max_state), now.c_str(),
        static_cast<unsigned>(post_type), static_cast<unsigned>(my_type), extra.c_str(), lim);
    std::vector<PostRow> out;
    for (const DBRow& r : rows) {
        PostRow p;
        p.date_seq    = to_u64(r, "date_seq");
        p.post_uid    = static_cast<uint32_t>(to_u64(r, "post_uid"));
        p.post_type   = static_cast<uint8_t>(to_i(r, "post_type"));
        p.sub_type    = static_cast<uint8_t>(to_i(r, "post_sub_type"));
        p.state       = static_cast<uint8_t>(to_i(r, "post_state"));
        p.user_type   = static_cast<uint8_t>(to_i(r, "user_type"));
        p.title       = to_s(r, "title");
        p.char_name   = to_s(r, "char_name");
        p.occur_date  = to_s(r, "send_date");
        p.return_date = to_s(r, "return_date");
        out.push_back(p);
    }
    return out;
}

int next_bag_slot(Database& db, uint32_t char_uid)
{
    auto r = db.query("SELECT COALESCE(MAX(bag_slot), 99) + 1 AS nxt FROM char_inventory WHERE char_uid=%u", char_uid);
    const int s = r.empty() ? 100 : to_i(r[0], "nxt");
    return s < 100 ? 100 : s;
}

// The rows a post's state change adds (the procedures' INSERT INTO PostDate SELECT .. FROM PostDate WHERE
// PostUID = @PostUID AND PostState = @old): one per side; the receiver's (UserType 1) ReturnDate a new one,
// the sender's kept; `recv_char` the receiver row's CharUID (0: kept). Returns the rows added.
uint64_t add_state_rows(Database& db, uint32_t post_uid, uint8_t old_state, uint8_t new_state, const std::string& now,
                        const std::string& recv_return, uint32_t recv_char)
{
    auto rows = db.query("SELECT user_type, user_uid, char_uid, char_name, char_level, return_date FROM post_date "
                         "WHERE post_uid = %u AND post_state = %u", post_uid, static_cast<unsigned>(old_state));
    uint64_t n = 0;
    for (const DBRow& r : rows) {
        const int ut = to_i(r, "user_type");
        const unsigned cu = (ut == 1 && recv_char) ? recv_char : static_cast<unsigned>(to_u64(r, "char_uid"));
        const std::string ret = (ut == 1) ? recv_return : to_s(r, "return_date");
        n += db.execute("INSERT INTO post_date (post_uid, post_state, user_type, user_uid, char_uid, char_name, "
                        "char_level, send_date, return_date, create_date) VALUES (%u, %u, %d, %u, %u, '%s', %d, '%s', "
                        "'%s', '%s')", post_uid, static_cast<unsigned>(new_state), ut,
                        static_cast<unsigned>(to_u64(r, "user_uid")), cu, db.escape(to_s(r, "char_name")).c_str(),
                        to_i(r, "char_level"), now.c_str(), ret.c_str(), now.c_str());
    }
    return n;
}

// USP_PostDate_Update without its own transaction (the callers' BEGIN TRAN holds it).
void expire_rows(Database& db, uint32_t user_uid, const std::string& now)
{
    auto rows = db.query(
        "SELECT a.post_uid, a.post_sub_type, a.post_state, a.user_type, a.user_uid, a.char_uid, a.char_name, "
        "a.char_level, a.return_date FROM ("
        " SELECT p.post_sub_type, d.date_seq, d.post_uid, d.post_state, d.user_type, d.user_uid, d.char_uid, "
        " d.char_name, d.char_level, d.return_date, "
        " RANK() OVER (PARTITION BY d.post_uid ORDER BY d.date_seq DESC) AS rn"
        " FROM post p JOIN post_date d ON p.post_uid = d.post_uid AND p.post_state < 3"
        " WHERE d.user_uid = %u) a "
        "WHERE a.post_state IN (0,1,2) AND a.rn = 1 AND a.return_date <= '%s'", user_uid, now.c_str());
    if (rows.empty()) return;
    struct E {
        uint32_t post_uid; int old_state, new_state, user_type; unsigned user, chr; std::string name; int level;
        std::string send, ret;
    };
    std::vector<E> es;
    for (const DBRow& r : rows) {
        E e;
        e.post_uid  = static_cast<uint32_t>(to_u64(r, "post_uid"));
        e.old_state = to_i(r, "post_state");
        e.user_type = to_i(r, "user_type");
        e.user      = static_cast<unsigned>(to_u64(r, "user_uid"));
        e.chr       = static_cast<unsigned>(to_u64(r, "char_uid"));
        e.name      = to_s(r, "char_name");
        e.level     = to_i(r, "char_level");
        const int sub = to_i(r, "post_sub_type");
        const std::string old_ret = to_s(r, "return_date");
        e.send = old_ret;
        if (sub != 3) {                                   // a payment is deleted, never returned
            e.new_state = (e.old_state == 0) ? 2 : 3;
            e.ret = (e.old_state == 0) ? PostStore::DateInfo(old_ret, 1) : PostStore::DateInfo(old_ret, 0);
        } else {
            e.new_state = 3;
            e.ret = PostStore::DateInfo(old_ret, 0);
        }
        es.push_back(e);
    }
    const size_t n0 = es.size();
    for (size_t i = 0; i < n0; ++i) {                     // returned and past its new date too: deleted as well
        if (es[i].new_state != 2 || es[i].ret > now) continue;
        E d = es[i];
        d.new_state = 3;
        d.send = es[i].ret;
        d.ret = PostStore::DateInfo(es[i].ret, 0);
        es.push_back(d);
    }
    for (const E& e : es) {
        if (e.new_state == 2) {
            // both sides' rows at the old state, the sender and receiver swapped (the mail goes back)
            db.execute("INSERT INTO post_date (post_uid, post_state, user_type, user_uid, char_uid, char_name, "
                       "char_level, send_date, return_date, create_date) "
                       "SELECT post_uid, 2, CASE WHEN user_type = 0 THEN 1 ELSE 0 END, user_uid, char_uid, char_name, "
                       "char_level, send_date, '%s', '%s' FROM post_date WHERE post_uid = %u AND post_state = %d",
                       e.ret.c_str(), now.c_str(), e.post_uid, e.old_state);
        } else {
            db.execute("INSERT INTO post_date (post_uid, post_state, user_type, user_uid, char_uid, char_name, "
                       "char_level, send_date, return_date, create_date) VALUES (%u, 3, %d, %u, %u, '%s', %d, '%s', "
                       "'%s', '%s')", e.post_uid, e.user_type, e.user, e.chr, db.escape(e.name).c_str(), e.level,
                       e.send.c_str(), e.ret.c_str(), now.c_str());
        }
    }
    // UPDATE Post SET PostState = MAX(NewPostState) .. GROUP BY PostUID HAVING COUNT(*) = 2 (the join of the
    // post's rows at the old state with its entries: two rows, one entry — a post with a second entry is left)
    std::map<uint32_t, std::pair<int, int>> per;          // post -> {entries, max new state}
    std::map<uint32_t, int> old_of;
    for (const E& e : es) {
        if (e.new_state < 2) continue;
        auto& v = per[e.post_uid];
        ++v.first;
        v.second = (std::max)(v.second, e.new_state);
        old_of[e.post_uid] = e.old_state;
    }
    for (const auto& kv : per) {
        auto c = db.query("SELECT COUNT(*) AS n FROM post_date WHERE post_uid = %u AND post_state = %d", kv.first,
                          old_of[kv.first]);
        const int rows_at_old = c.empty() ? 0 : to_i(c[0], "n");
        if (rows_at_old * kv.second.first == 2)
            db.execute("UPDATE post SET post_state = %d WHERE post_uid = %u", kv.second.second, kv.first);
    }
}

// The receiver of a name (User_Info x Char_Attr x Char_Status; a renamed deleted character is not one).
bool char_by_name(Database& db, const std::string& name, uint32_t& user, uint32_t& chr, int& level)
{
    auto r = db.query("SELECT uid, owner, level FROM characters WHERE name = '%s' AND deleted = 0",
                      db.escape(name).c_str());
    if (r.size() != 1) return false;
    chr   = static_cast<uint32_t>(to_u64(r[0], "uid"));
    user  = static_cast<uint32_t>(to_u64(r[0], "owner"));
    level = to_i(r[0], "level");
    return true;
}

}  // namespace

std::string PostStore::Now()
{
    const std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return fmt_tm(lt);
}

// FN_PostDateInfo: the date to the minute, then 0 -> 2050-01-01, 1 -> + 7 days, 2 -> + 1 day, else + 3 days.
std::string PostStore::DateInfo(const std::string& from, int type)
{
    if (type == 0) return "2050-01-01 00:00:00";
    std::tm t{};
    if (!parse_tm(from, t)) return from;
    t.tm_sec = 0;
    t.tm_mday += (type == 1) ? 7 : (type == 2) ? 1 : 3;
    std::mktime(&t);
    return fmt_tm(t);
}

int PostStore::Expire(uint32_t user_uid)
{
    DBTransaction tx(db_);
    expire_rows(db_, user_uid, Now());
    tx.commit();
    return 0;
}

int PostStore::Count(uint32_t char_uid, int& fresh, int& returned)
{
    fresh = returned = 0;
    auto c = db_.query("SELECT owner FROM characters WHERE uid = %u", char_uid);
    if (c.empty()) return 101;
    const uint32_t user = static_cast<uint32_t>(to_u64(c[0], "owner"));
    const std::string now = Now();
    {
        DBTransaction tx(db_);
        expire_rows(db_, user, now);
        tx.commit();
    }
    auto rows = db_.query(
        "SELECT a.post_type, a.post_state, a.char_uid FROM ("
        " SELECT p.post_type, d.post_uid, d.post_state, d.return_date, d.char_uid, d.user_type, "
        " RANK() OVER (PARTITION BY d.post_uid ORDER BY d.date_seq DESC) AS rn"
        " FROM post p JOIN post_date d ON p.post_uid = d.post_uid AND p.post_state < 3"
        " WHERE d.user_uid = %u) a "
        "WHERE a.rn = 1 AND a.post_state IN (0,2) AND a.return_date > '%s' AND a.user_type = 1", user, now.c_str());
    for (const DBRow& r : rows) {
        const int st = to_i(r, "post_state"), pt = to_i(r, "post_type");
        if (st == 0 && ((pt == 0 && to_u64(r, "char_uid") == char_uid) || pt == 1)) ++fresh;
        if (st == 2) ++returned;
    }
    return 0;
}

int PostStore::List(uint8_t list_type, uint32_t user_uid, uint32_t char_uid, uint64_t date_seq, int rows,
                    std::vector<PostRow>& out)
{
    const std::string now = Now();
    char extra[48] = "";
    if (date_seq) std::snprintf(extra, sizeof(extra), " AND a.date_seq < %llu", static_cast<unsigned long long>(date_seq));
    if (list_type == 0)      out = latest_rows(db_, user_uid, char_uid, 0, 1, 0, 2, now, extra, rows);   // received
    else if (list_type == 1) out = latest_rows(db_, user_uid, char_uid, 0, 0, 1, 1, now, extra, rows);   // sent
    else if (list_type == 2) out = latest_rows(db_, user_uid, 0, 1, 1, 0, 1, now, extra, rows);          // the gift box
    else out.clear();
    return 0;
}

int PostStore::Sync(uint32_t user_uid, uint32_t char_uid, uint64_t date_seq, int& fresh, int& returned,
                    std::vector<PostRow>& box, std::vector<PostRow>& gifts)
{
    fresh = returned = 0;
    box.clear();
    gifts.clear();
    if (db_.query("SELECT date_seq FROM post_date WHERE user_uid = %u LIMIT 1", user_uid).empty()) return 0;
    const std::string now = Now();
    {
        DBTransaction tx(db_);
        expire_rows(db_, user_uid, now);
        tx.commit();
    }
    std::vector<PostRow> all = latest_rows(db_, user_uid, char_uid, 0, 1, 0, 2, now, "", 0);
    std::vector<PostRow> sent = latest_rows(db_, user_uid, char_uid, 0, 0, 1, 1, now, "", 0);
    std::vector<PostRow> gift = latest_rows(db_, user_uid, 0, 1, 1, 0, 1, now, "", 0);
    all.insert(all.end(), sent.begin(), sent.end());
    all.insert(all.end(), gift.begin(), gift.end());
    std::sort(all.begin(), all.end(), [](const PostRow& a, const PostRow& b) { return a.date_seq > b.date_seq; });
    for (const PostRow& p : all) {
        if (p.user_type != 1) continue;
        if (p.state == 0) ++fresh;
        if (p.state == 2) ++returned;
    }
    for (const PostRow& p : all) {
        if (date_seq && p.date_seq <= date_seq) continue;
        if (p.post_type == 0) box.push_back(p);
        else if (p.post_type == 1 && p.user_type == 1) gifts.push_back(p);
    }
    return 0;
}

int PostStore::Detail(uint32_t post_uid, uint32_t user_uid, uint32_t char_uid, std::string& content, int32_t& money,
                      std::vector<PostItemRow>& items, std::vector<PostOptionRow>& opts)
{
    (void)user_uid;
    content.clear();
    money = 0;
    items.clear();
    opts.clear();
    const std::string now = Now();
    auto p = db_.query("SELECT p.post_type, p.post_sub_type, p.post_state, p.content, p.post_money, b.user_type, "
                       "b.send_date, b.return_date FROM post p JOIN post_date b ON p.post_uid = b.post_uid AND "
                       "p.post_state = b.post_state AND b.char_uid <> %u WHERE p.post_uid = %u "
                       "ORDER BY b.date_seq DESC LIMIT 1", char_uid, post_uid);
    if (p.empty()) return 101;                                   // 'Not Found PostInfo'
    const int type = to_i(p[0], "post_type"), sub = to_i(p[0], "post_sub_type");
    const int state = to_i(p[0], "post_state"), other = to_i(p[0], "user_type");
    content = to_s(p[0], "content");
    money   = to_i(p[0], "post_money");
    if (sub == 3 && state == 2) money = 0;
    if (type != 0) return 102;                                   // 'PostType Value Error'
    if (type == 0 && sub == 1 &&
        (to_s(p[0], "send_date") > now || to_s(p[0], "return_date") < now)) return 103;   // 'ReturnDate Period Errors'
    if (sub == 0 && (state == 0 || state == 2) && other == 0) {
        // the receiver read a normal post: read (its ReturnDate 7 days from now; the sender's row keeps its own)
        DBTransaction tx(db_);
        db_.execute("UPDATE post SET post_state = 1 WHERE post_uid = %u", post_uid);
        if (db_.query("SELECT post_uid FROM post WHERE post_uid = %u AND post_state = 1", post_uid).size() != 1) return 103;
        if (add_state_rows(db_, post_uid, static_cast<uint8_t>(state), 1, now, DateInfo(now, 1), 0) != 2) return 118;
        tx.commit();
    }
    auto it = db_.query("SELECT i.item_seq, i.item_uid, i.item_class, i.item_index, i.item_attr, i.add_minute, "
                        "i.item_flag, COALESCE(c.refine, 0) AS refine, COALESCE(c.refine_state, 0) AS refine_state, "
                        "COALESCE(c.item_slot, 0) AS item_slot FROM post_item i LEFT JOIN char_inventory c "
                        "ON i.item_class = 2 AND i.item_uid > 0 AND c.uid = i.item_uid WHERE i.post_uid = %u "
                        "ORDER BY i.item_seq", post_uid);
    for (const DBRow& r : it) {
        PostItemRow x;
        x.item_seq   = to_u64(r, "item_seq");
        x.item_uid   = to_u64(r, "item_uid");
        x.item_class = static_cast<uint8_t>(to_i(r, "item_class"));
        x.item_index = static_cast<uint32_t>(to_u64(r, "item_index"));
        x.item_attr  = static_cast<int16_t>(to_i(r, "item_attr"));
        x.add_minute = to_i(r, "add_minute");
        x.item_flag  = static_cast<uint8_t>(to_i(r, "item_flag"));
        if (x.item_class == 2 && x.item_uid > 0) {
            x.refine       = static_cast<uint8_t>(to_i(r, "refine"));
            x.refine_state = static_cast<uint8_t>(to_i(r, "refine_state"));
            x.item_slot    = static_cast<uint8_t>(to_i(r, "item_slot"));
        }
        items.push_back(x);
    }
    auto op = db_.query("SELECT i.item_seq, o.item_uid, o.option_type, o.option_index, o.option_value, o.mixed_item "
                        "FROM post_item i JOIN char_item_option o ON o.item_uid = i.item_uid WHERE i.post_uid = %u "
                        "AND i.item_uid > 0", post_uid);
    for (const DBRow& r : op) {
        PostOptionRow o;
        o.item_seq = to_u64(r, "item_seq");
        o.item_uid = to_u64(r, "item_uid");
        o.type     = static_cast<uint8_t>(to_i(r, "option_type"));
        o.index    = static_cast<int16_t>(to_i(r, "option_index"));
        o.value    = static_cast<int16_t>(to_i(r, "option_value"));
        o.mixed    = to_i(r, "mixed_item");
        opts.push_back(o);
    }
    return 0;
}

int PostStore::GiftDetail(uint32_t post_uid, uint32_t user_uid, uint32_t char_uid, std::string& content,
                          std::vector<PostItemRow>& items)
{
    content.clear();
    items.clear();
    const std::string now = Now();
    auto p = db_.query("SELECT p.post_type, p.post_state, p.content FROM post p JOIN post_date b ON "
                       "p.post_uid = b.post_uid AND p.post_state = b.post_state AND b.user_uid <> %u "
                       "WHERE p.post_uid = %u ORDER BY b.date_seq DESC LIMIT 1", user_uid, post_uid);
    if (p.empty()) return 101;
    if (to_i(p[0], "post_type") != 1) return 102;
    const int state = to_i(p[0], "post_state");
    content = to_s(p[0], "content");
    auto n = db_.query("SELECT COUNT(*) AS n FROM post_item WHERE post_uid = %u", post_uid);
    if ((n.empty() ? 0 : to_i(n[0], "n")) == 0 && state == 0) {
        DBTransaction tx(db_);
        db_.execute("UPDATE post SET post_state = 1 WHERE post_uid = %u", post_uid);
        if (add_state_rows(db_, post_uid, 0, 1, now, DateInfo(now, 1), char_uid) != 2) return 101;
        tx.commit();
    }
    auto it = db_.query("SELECT item_seq, item_index, item_class, item_attr, add_minute, item_flag FROM post_item "
                        "WHERE post_uid = %u ORDER BY item_seq", post_uid);
    for (const DBRow& r : it) {
        PostItemRow x;
        x.item_seq   = to_u64(r, "item_seq");
        x.item_class = static_cast<uint8_t>(to_i(r, "item_class"));
        x.item_index = static_cast<uint32_t>(to_u64(r, "item_index"));
        x.item_attr  = static_cast<int16_t>(to_i(r, "item_attr"));
        x.add_minute = to_i(r, "add_minute");
        x.item_flag  = static_cast<uint8_t>(to_i(r, "item_flag"));
        items.push_back(x);
    }
    return 0;
}

int PostStore::Send(const SendArgs& a, const std::vector<PostItemOp>& from, const std::vector<PostItemOp>& to,
                    uint32_t& post_uid)
{
    post_uid = 0;
    const std::string now = Now();
    std::string ret;
    if (a.sub_type == 0 || a.sub_type == 1) ret = DateInfo(now, 1);
    else if (a.sub_type == 2) ret = DateInfo(now, 2);
    else return 101;                                              // 'Post SubType Value Error'
    uint32_t to_user = 0, to_char = 0;
    int to_level = 0;
    if (!char_by_name(db_, a.to_name, to_user, to_char, to_level)) return 102;   // 'Not Found ToCharName'
    if (a.from_user == to_user || a.from_char == to_char) return 103;            // 'Send to myself Error'
    if (a.money < 0 || a.tax < 0 || a.money > 500000000) return 104;             // 'Gelder over flow'
    {
        const std::string day = now.substr(0, 10);
        auto c = db_.query("SELECT COUNT(*) AS n FROM post p JOIN post_date d ON p.post_uid = d.post_uid WHERE "
                           "d.user_uid = %u AND d.char_uid = %u AND d.user_type = 0 AND p.create_date >= '%s 00:00:00' "
                           "AND p.create_date <= '%s 23:59:59'", a.from_user, a.from_char, day.c_str(), day.c_str());
        if ((c.empty() ? 0 : to_i(c[0], "n")) >= 25) return 105;              // 'Send to 1 day 25 times limited'
    }
    {
        auto c = db_.query(
            "SELECT COUNT(*) AS n FROM ("
            " SELECT p.post_sub_type, d.post_state, d.return_date, d.user_type, "
            " RANK() OVER (PARTITION BY d.post_uid ORDER BY d.date_seq DESC) AS rn"
            " FROM post p JOIN post_date d ON p.post_uid = d.post_uid AND p.post_state < 3"
            " WHERE d.user_uid = %u AND d.char_uid = %u AND p.post_type = 0) a "
            "WHERE a.post_state IN (0,2) AND a.post_sub_type <> 3 AND a.rn = 1 AND a.return_date > '%s' "
            "AND a.user_type = 1", to_user, to_char, now.c_str());
        if ((c.empty() ? 0 : to_i(c[0], "n")) >= 100) return 106;             // the receiver's box is full
    }
    DBTransaction tx(db_);
    if (a.sub_type == 1 && (a.money > 0 || a.tax > 0)) {          // a gift's gelders + fee leave the sender
        if (db_.query("SELECT uid FROM characters WHERE uid = %u", a.from_char).empty()) return 107;
        if (a.from_money_final < 0) return 108;                   // 'Not enough Money'
        db_.execute("UPDATE characters SET money = %d WHERE uid = %u", a.from_money_final, a.from_char);
    }
    post_uid = static_cast<uint32_t>(db_.insert(
        "INSERT INTO post (post_type, post_sub_type, post_state, title, content, post_money_tax, post_money, "
        "post_money_take_date, sell_code, parent_post_uid, create_date) VALUES (0, %u, 0, '%s', '%s', %d, %d, NULL, 0, "
        "0, '%s')", static_cast<unsigned>(a.sub_type), db_.escape(a.title).c_str(), db_.escape(a.content).c_str(),
        a.tax, a.money, now.c_str()));
    if (!post_uid) return 109;
    db_.execute("INSERT INTO post_date (post_uid, post_state, user_type, user_uid, char_uid, char_name, char_level, "
                "send_date, return_date, create_date) VALUES (%u, 0, 0, %u, %u, '%s', %d, '%s', '%s', '%s')",
                post_uid, a.from_user, a.from_char, db_.escape(a.from_name).c_str(), a.from_level, now.c_str(),
                ret.c_str(), now.c_str());
    if (db_.execute("INSERT INTO post_date (post_uid, post_state, user_type, user_uid, char_uid, char_name, char_level, "
                    "send_date, return_date, create_date) VALUES (%u, 0, 1, %u, %u, '%s', %d, '%s', '%s', '%s')",
                    post_uid, to_user, to_char, db_.escape(a.to_name).c_str(), a.from_level, now.c_str(), ret.c_str(),
                    now.c_str()) != 1) return 109;                // 'INSERT PostDate Fail'
    if (!from.empty() || !to.empty()) {
        // the stream lists: every row counts, a LastCount 0 one is dropped first (then the counts differ)
        const size_t rows = from.size() + to.size();
        size_t done = 0;
        for (const PostItemOp& e : from) {
            if (e.count == 0) continue;
            const unsigned long long u = static_cast<unsigned long long>(e.uid);
            if (e.flag == 0) {                                    // DELETE (common / rare): the bag lets it go
                done += static_cast<size_t>(db_.execute("UPDATE char_inventory SET char_uid = 0, bag_slot = -1 WHERE "
                                                        "uid = %llu AND char_uid = %u", u, a.from_char));
            } else if (e.flag == 2 && e.cls == 1) {               // UPDATE (common): the stack keeps the rest
                db_.execute("UPDATE char_inventory SET amount = %u WHERE uid = %llu", static_cast<unsigned>(e.count), u);
                done += db_.query("SELECT uid FROM char_inventory WHERE uid = %llu AND amount = %u", u,
                                  static_cast<unsigned>(e.count)).size();
            }
        }
        for (const PostItemOp& e : to) {
            if (e.count == 0 || e.flag != 1) continue;
            const int flag = (e.cls == 2 && e.count == 1) ? 0 : (e.cls == 1) ? 2 : (e.cls == 2 && e.count > 1) ? 2 : 0;
            done += static_cast<size_t>(db_.execute(
                "INSERT INTO post_item (post_uid, item_uid, item_class, item_index, item_attr, add_minute, item_flag, "
                "create_date) VALUES (%u, %llu, %u, %u, %u, 0, %d, '%s')", post_uid,
                static_cast<unsigned long long>(e.uid), static_cast<unsigned>(e.cls), e.index,
                static_cast<unsigned>(e.count), flag, now.c_str()));
        }
        if (rows != done) return 110;                             // 'updated rows of items have different item_rows'
    }
    tx.commit();
    return 0;
}

int PostStore::Recv(const RecvArgs& a, std::vector<PostItemOp>& ops)
{
    const std::string now = Now();
    DBTransaction tx(db_);
    expire_rows(db_, a.to_user, now);
    auto p = db_.query("SELECT p.post_type, p.post_sub_type, p.post_state, p.post_money, p.post_money_tax, "
                       "p.post_money_take_date, b.user_uid, b.char_uid, b.char_name, b.send_date, b.return_date, "
                       "c.level, c.deleted FROM post p JOIN post_date b ON p.post_uid = b.post_uid AND "
                       "p.post_state = b.post_state LEFT JOIN characters c ON c.uid = b.char_uid "
                       "WHERE p.post_uid = %u AND b.user_type = 0 ORDER BY b.date_seq DESC LIMIT 1", a.post_uid);
    if (p.empty()) return 101;                                     // 'Not Found PostInfo'
    const int type = to_i(p[0], "post_type"), sub = to_i(p[0], "post_sub_type");
    const int ck_state = to_i(p[0], "post_state");
    const int ck_money = to_i(p[0], "post_money"), ck_tax = to_i(p[0], "post_money_tax");
    const bool taken = !to_s(p[0], "post_money_take_date").empty();
    const unsigned from_user = static_cast<unsigned>(to_u64(p[0], "user_uid"));
    const unsigned from_char = static_cast<unsigned>(to_u64(p[0], "char_uid"));
    const std::string from_name = to_s(p[0], "char_name");
    const int from_level = to_i(p[0], "level");
    if (ck_state != a.state) return 102;                          // 'PostState Value Check Error'
    if (type == 0 && sub == 1 && (to_s(p[0], "send_date") > now || to_s(p[0], "return_date") < now)) return 103;
    bool money_changed = false;
    if (sub == 2 && a.money > 0 && !taken) {
        if (to_i(p[0], "deleted") != 0) return 105;               // 'FromChar Deleted'
        if (ck_state == 0) {
            // the receiver pays the requested gelders; a payment post (sub type 3) takes them, less the fee, to
            // the sender (ParentPostUID = this post)
            if (a.to_money_final < 0) return 108;                 // 'Not enough Money'
            money_changed = true;
            const std::string nret = DateInfo(now, 1);
            const uint32_t np = static_cast<uint32_t>(db_.insert(
                "INSERT INTO post (post_type, post_sub_type, post_state, title, content, post_money_tax, post_money, "
                "post_money_take_date, sell_code, parent_post_uid, create_date) VALUES (0, 3, 0, '%s', '%s', 0, %d, "
                "NULL, 0, %u, '%s')", db_.escape(a.sys_title).c_str(), db_.escape(a.sys_content).c_str(),
                ck_money - ck_tax, a.post_uid, now.c_str()));
            if (!np) return 109;
            db_.execute("INSERT INTO post_date (post_uid, post_state, user_type, user_uid, char_uid, char_name, char_level, "
                        "send_date, return_date, create_date) VALUES (%u, 0, 0, %u, %u, '%s', %d, '%s', '%s', '%s')",
                        np, a.to_user, a.to_char, db_.escape(a.to_name).c_str(), static_cast<int>(a.to_level), now.c_str(),
                        nret.c_str(), now.c_str());
            if (db_.execute("INSERT INTO post_date (post_uid, post_state, user_type, user_uid, char_uid, char_name, "
                            "char_level, send_date, return_date, create_date) VALUES (%u, 0, 1, %u, %u, '%s', %d, '%s', "
                            "'%s', '%s')", np, from_user, from_char, db_.escape(from_name).c_str(), from_level,
                            now.c_str(), nret.c_str(), now.c_str()) != 1) return 109;   // 'INSERT PostDate Fail'
        }
    } else if ((sub == 1 || sub == 3) && a.money > 0 && !taken) {
        if (a.to_money_final < 0) return 110;
        money_changed = true;                                     // the gift's / payment's gelders come in
    }
    if (money_changed) {
        if (db_.query("SELECT uid FROM characters WHERE uid = %u", a.to_char).empty()) return 107;
        db_.execute("UPDATE characters SET money = %d WHERE uid = %u", a.to_money_final, a.to_char);
    }
    if (!ops.empty()) {
        const size_t rows = ops.size();
        uint64_t upd = 0;
        for (const PostItemOp& e : ops)                           // UPDATE (common): joins a stack of the bag
            if (e.cls == 1 && e.flag == 2) {
                db_.execute("UPDATE char_inventory SET amount = %u WHERE uid = %llu", static_cast<unsigned>(e.count),
                            static_cast<unsigned long long>(e.uid));
                upd += db_.query("SELECT uid FROM char_inventory WHERE uid = %llu AND amount = %u",
                                 static_cast<unsigned long long>(e.uid), static_cast<unsigned>(e.count)).size();
            }
        for (PostItemOp& e : ops) {
            if (e.flag != 1) continue;                            // INSERT
            if (e.cls == 1) {
                e.new_uid = db_.insert("INSERT INTO char_inventory (char_uid, item_id, amount, bag_slot, equip_slot, "
                                       "item_where, item_slot) VALUES (%u, %u, %u, %d, -1, 1, %u)", a.to_char, e.index,
                                       static_cast<unsigned>(e.count), next_bag_slot(db_, a.to_char),
                                       static_cast<unsigned>(e.slot));
                upd = e.new_uid ? 1 : 0;                          // (SET @ItemUpdateRows = @@ROWCOUNT: the
                if (upd != 1) return 111;                         //   retail overwrites the count here)
            } else if (e.uid > 0 && e.cls == 2) {
                upd = db_.execute("UPDATE char_inventory SET char_uid = %u, bag_slot = %d, item_where = 1, equip_slot = -1 "
                                  "WHERE uid = %llu AND char_uid = 0", a.to_char, next_bag_slot(db_, a.to_char),
                                  static_cast<unsigned long long>(e.uid));
                if (upd != 1) return 112;                         // 'UPDATE RARE Tbl_Item_Char Fail'
                e.new_uid = e.uid;
            }
        }
        // 'updated rows of items have different'. Retail as it is: both INSERT branches overwrite the count
        // (USPM_Recv_PostItem: SET @ItemUpdateRows = @@ROWCOUNT), so a mail of 2+ rows with any INSERT row (a new
        // stack or a rare item) ends here — the retail server fails it too (doc/FEATURES.md, known issues).
        if (rows != upd) return 114;
        uint64_t took = 0;
        for (const PostItemOp& e : ops)
            took += db_.execute("UPDATE post_item SET take_char_uid = %u, take_char_level = %d, take_date = '%s', "
                                "item_uid = %llu WHERE item_seq = %llu", a.to_char, static_cast<int>(a.to_level),
                                now.c_str(), static_cast<unsigned long long>(e.new_uid ? e.new_uid : e.uid),
                                static_cast<unsigned long long>(e.item_seq));
        if (took != rows) return 115;
    }
    if (a.money == 0) {
        db_.execute("UPDATE post SET post_state = 1 WHERE post_uid = %u", a.post_uid);
        if (db_.query("SELECT post_uid FROM post WHERE post_uid = %u", a.post_uid).size() != 1) return 116;
    } else {
        if (db_.execute("UPDATE post SET post_state = 1, post_money_take_date = '%s' WHERE post_uid = %u AND "
                        "post_money_take_date IS NULL", now.c_str(), a.post_uid) != 1) return 117;   // 'Aleady Update ..'
    }
    if (add_state_rows(db_, a.post_uid, a.state, 1, now, DateInfo(now, 1), 0) != 2) return 118;
    tx.commit();
    return 0;
}

int PostStore::RecvGift(const RecvArgs& a, std::vector<PostItemOp>& ops, const std::vector<PostOptionRow>& opts)
{
    const std::string now = Now();
    DBTransaction tx(db_);
    auto p = db_.query(
        "SELECT a.post_state FROM ("
        " SELECT p.post_type, d.post_uid, d.post_state, d.user_type, d.return_date, "
        " RANK() OVER (PARTITION BY d.post_uid ORDER BY d.date_seq DESC) AS rn"
        " FROM post p JOIN post_date d ON p.post_uid = d.post_uid AND p.post_state < 3"
        " WHERE d.user_uid = %u AND p.post_uid = %u) a "
        "JOIN post_date c ON a.post_uid = c.post_uid AND a.post_state = c.post_state AND c.user_type = 0 "
        "WHERE a.rn = 1 AND a.post_state = 0 AND a.user_type = 1 AND a.return_date > '%s' LIMIT 1",
        a.to_user, a.post_uid, now.c_str());
    if (p.empty()) return 101;                                     // 'Not Found PostInfo'
    if (!ops.empty()) {
        const size_t rows = ops.size();
        uint64_t upd = 0;
        for (const PostItemOp& e : ops)
            if (e.cls == 1 && e.flag == 2) {
                db_.execute("UPDATE char_inventory SET amount = %u WHERE uid = %llu", static_cast<unsigned>(e.count),
                            static_cast<unsigned long long>(e.uid));
                upd += db_.query("SELECT uid FROM char_inventory WHERE uid = %llu AND amount = %u",
                                 static_cast<unsigned long long>(e.uid), static_cast<unsigned>(e.count)).size();
            }
        for (PostItemOp& e : ops) {
            if (e.flag != 1) continue;
            auto it = db_.query("SELECT item_index, item_class, item_attr FROM post_item WHERE item_seq = %llu",
                                static_cast<unsigned long long>(e.item_seq));
            if (it.empty()) continue;
            const unsigned idx = static_cast<unsigned>(to_u64(it[0], "item_index"));
            const int cls = to_i(it[0], "item_class"), attr = to_i(it[0], "item_attr");
            e.new_uid = db_.insert("INSERT INTO char_inventory (char_uid, item_id, amount, bag_slot, equip_slot, item_where, "
                                   "item_slot) VALUES (%u, %u, %d, %d, -1, 1, %u)", a.to_char, idx, attr,
                                   next_bag_slot(db_, a.to_char), static_cast<unsigned>(e.slot));
            if (!e.new_uid) return 103;                            // 'INSERT Tbl_Item_Char Fail'
            ++upd;
            if (cls == 2)
                for (const PostOptionRow& o : opts)                // its options (type 10)
                    if (o.item_seq == e.item_seq)
                        db_.execute("INSERT INTO char_item_option (item_uid, option_type, option_index, option_value, "
                                    "mixed_item) VALUES (%llu, 10, %d, %d, %d)", static_cast<unsigned long long>(e.new_uid),
                                    static_cast<int>(o.index), static_cast<int>(o.value), o.mixed);
        }
        if (rows != upd) return 103;                              // 'updated rows of items have different'
        uint64_t took = 0;
        for (const PostItemOp& e : ops)
            took += db_.execute("UPDATE post_item SET take_char_uid = %u, take_char_level = %d, take_date = '%s', "
                                "item_uid = %llu WHERE item_seq = %llu", a.to_char, static_cast<int>(a.to_level),
                                now.c_str(), static_cast<unsigned long long>(e.new_uid ? e.new_uid : e.uid),
                                static_cast<unsigned long long>(e.item_seq));
        if (took != rows) return 103;                             // '.. different2'
    }
    if (!db_.query("SELECT item_seq FROM post_item WHERE post_uid = %u AND take_date IS NULL LIMIT 1",
                   a.post_uid).empty()) return 101;               // 'PostItem TakeDate Insert Fail'
    db_.execute("UPDATE post SET post_state = 1 WHERE post_uid = %u", a.post_uid);
    if (add_state_rows(db_, a.post_uid, 0, 1, now, DateInfo(now, 1), a.to_char) != 2) return 101;
    tx.commit();
    return 0;
}

int PostStore::Delete(uint32_t post_uid, uint32_t user_uid, uint32_t char_uid, uint64_t del_seq, uint64_t last_seq,
                      std::vector<PostRow>& next)
{
    next.clear();
    const std::string now = Now();
    DBTransaction tx(db_);
    expire_rows(db_, user_uid, now);
    auto pt = db_.query("SELECT post_type FROM post WHERE post_uid = %u", post_uid);
    const int ptype = pt.empty() ? 0 : to_i(pt[0], "post_type");
    char who[48] = "";
    if (ptype == 0) std::snprintf(who, sizeof(who), " AND d.char_uid = %u", char_uid);
    auto r = db_.query(
        "SELECT a.post_type, a.post_sub_type, a.post_state, a.user_type FROM ("
        " SELECT p.post_type, p.post_sub_type, d.date_seq, d.post_uid, d.post_state, d.user_type, d.return_date, "
        " RANK() OVER (PARTITION BY d.post_uid ORDER BY d.date_seq DESC) AS rn"
        " FROM post p JOIN post_date d ON p.post_uid = d.post_uid AND p.post_state < 3"
        " WHERE d.user_uid = %u AND d.post_uid = %u%s) a "
        "WHERE a.rn = 1 AND a.return_date > '%s' LIMIT 1", user_uid, post_uid, who, now.c_str());
    if (r.empty()) return 101;                                     // 'Not Found PostInfo'
    const int type = to_i(r[0], "post_type"), sub = to_i(r[0], "post_sub_type");
    const int state = to_i(r[0], "post_state"), ut = to_i(r[0], "user_type");
    int del = 0;                                                   // 0 not deletable, 1 deleted, 2 returned
    if (ut == 1) {
        if (state == 0) del = (type == 0) ? ((sub <= 2) ? 2 : 0) : 0;
        else if (state == 1) del = 1;
        else del = 0;
    } else if (ut == 0) {
        del = 1;
    }
    if (del == 0) return 101;                                      // 'Delete Post Fail'
    if (del == 1) {
        db_.execute("INSERT INTO post_date (post_uid, post_state, user_type, user_uid, char_uid, char_name, char_level, "
                    "send_date, return_date, create_date) SELECT post_uid, 3, user_type, user_uid, char_uid, char_name, "
                    "char_level, '%s', '%s', '%s' FROM post_date WHERE date_seq = %llu AND post_uid = %u", now.c_str(),
                    DateInfo(now, 0).c_str(), now.c_str(), static_cast<unsigned long long>(del_seq), post_uid);
        if (type == 0) {
            auto c = db_.query("SELECT COUNT(*) AS n FROM post_date WHERE post_uid = %u AND post_state = 3", post_uid);
            if ((c.empty() ? 0 : to_i(c[0], "n")) == 2)            // both sides deleted it
                db_.execute("UPDATE post SET post_state = 3 WHERE post_uid = %u", post_uid);
        } else {
            db_.execute("UPDATE post SET post_state = 3 WHERE post_uid = %u", post_uid);
        }
    } else {
        // returned: the unread rows again, the sender and receiver swapped, for 7 days
        db_.execute("INSERT INTO post_date (post_uid, post_state, user_type, user_uid, char_uid, char_name, char_level, "
                    "send_date, return_date, create_date) SELECT post_uid, 2, CASE WHEN user_type = 0 THEN 1 ELSE 0 END, "
                    "user_uid, char_uid, char_name, char_level, '%s', '%s', '%s' FROM post_date WHERE post_uid = %u AND "
                    "post_state = 0", now.c_str(), DateInfo(now, 1).c_str(), now.c_str(), post_uid);
        db_.execute("UPDATE post SET post_state = 2 WHERE post_uid = %u", post_uid);
    }
    // the next older received row of that box (TOP 1 by PostUID DESC; the procedure looks at UserType 1 only)
    char who2[48] = "";
    if (ptype == 0) std::snprintf(who2, sizeof(who2), " AND d.char_uid = %u", char_uid);
    auto n = db_.query(
        "SELECT a.date_seq, a.post_uid, a.post_type, a.post_sub_type, a.post_state, a.user_type, a.title, c.char_name, "
        "a.send_date, a.return_date FROM (SELECT * FROM ("
        " SELECT p.post_type, p.post_sub_type, p.title, d.date_seq, d.post_uid, d.post_state, d.user_type, d.send_date, "
        " d.return_date, RANK() OVER (PARTITION BY d.post_uid ORDER BY d.date_seq DESC) AS rn"
        " FROM post p JOIN post_date d ON p.post_uid = d.post_uid AND p.post_state < 3"
        " WHERE d.user_uid = %u%s AND d.post_uid < %u AND d.date_seq < %llu) x "
        " WHERE x.rn = 1 AND x.post_state <= 2 AND x.return_date > '%s' AND x.user_type = 1 "
        " ORDER BY x.post_uid DESC LIMIT 1) a "
        "JOIN post_date c ON a.post_uid = c.post_uid AND a.post_state = c.post_state AND c.user_type = 0",
        user_uid, who2, post_uid, static_cast<unsigned long long>(last_seq), now.c_str());
    for (const DBRow& x : n) {
        PostRow p;
        p.date_seq    = to_u64(x, "date_seq");
        p.post_uid    = static_cast<uint32_t>(to_u64(x, "post_uid"));
        p.post_type   = static_cast<uint8_t>(to_i(x, "post_type"));
        p.sub_type    = static_cast<uint8_t>(to_i(x, "post_sub_type"));
        p.state       = static_cast<uint8_t>(to_i(x, "post_state"));
        p.user_type   = static_cast<uint8_t>(to_i(x, "user_type"));
        p.title       = to_s(x, "title");
        p.char_name   = to_s(x, "char_name");
        p.occur_date  = to_s(x, "send_date");
        p.return_date = to_s(x, "return_date");
        next.push_back(p);
    }
    tx.commit();
    return 0;
}

int PostStore::SendGMGift(const std::string& title, const std::string& content, uint32_t to_char,
                          const std::string& gm_name, const std::vector<GiftItem>& items)
{
    const std::string now = Now();
    const std::string ret = DateInfo(now, 0);
    auto c = db_.query("SELECT uid, owner, name, level FROM characters WHERE uid = %u AND deleted = 0", to_char);
    if (c.size() != 1) return 102;                                 // 'Not Found ToCharName'
    const unsigned to_user = static_cast<unsigned>(to_u64(c[0], "owner"));
    const std::string to_name = to_s(c[0], "name");
    const int to_level = to_i(c[0], "level");
    DBTransaction tx(db_);
    const uint32_t post_uid = static_cast<uint32_t>(db_.insert(
        "INSERT INTO post (post_type, post_sub_type, post_state, title, content, post_money_tax, post_money, "
        "post_money_take_date, sell_code, parent_post_uid, create_date) VALUES (1, 5, 0, '%s', '%s', 0, 0, NULL, 0, 0, "
        "'%s')", db_.escape(title).c_str(), db_.escape(content).c_str(), now.c_str()));
    if (!post_uid) return 109;
    db_.execute("INSERT INTO post_date (post_uid, post_state, user_type, user_uid, char_uid, char_name, char_level, "
                "send_date, return_date, create_date) VALUES (%u, 0, 0, 0, 0, '%s', 1, '%s', '%s', '%s')", post_uid,
                db_.escape(gm_name).c_str(), now.c_str(), ret.c_str(), now.c_str());
    if (db_.execute("INSERT INTO post_date (post_uid, post_state, user_type, user_uid, char_uid, char_name, char_level, "
                    "send_date, return_date, create_date) VALUES (%u, 0, 1, %u, %u, '%s', %d, '%s', '%s', '%s')", post_uid,
                    to_user, to_char, db_.escape(to_name).c_str(), to_level, now.c_str(), ret.c_str(), now.c_str()) != 1)
        return 109;                                                // 'INSERT PostDate Fail'
    size_t done = 0;
    for (const GiftItem& g : items) {
        const int add = (g.cls == 2 && g.add_minute > 0) ? g.add_minute : 0;
        const int flag = (g.cls == 2 && g.attr == 1 && g.add_minute == 0) ? 0 : (g.cls == 2 && g.add_minute > 0) ? 1
                       : (g.cls == 1) ? 2 : (g.cls == 2 && g.attr > 1) ? 2 : 0;
        done += static_cast<size_t>(db_.execute(
            "INSERT INTO post_item (post_uid, item_uid, item_class, item_index, item_attr, add_minute, item_flag, "
            "create_date) VALUES (%u, 0, %u, %u, %d, %d, %d, '%s')", post_uid, static_cast<unsigned>(g.cls), g.index,
            static_cast<int>(g.attr), add, flag, now.c_str()));
    }
    if (done != items.size()) return 110;
    tx.commit();
    return 0;
}
