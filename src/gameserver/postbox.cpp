#include "game_session.hpp"
#include "game_server.hpp"
#include "client_text.hpp"
#include "filetime.hpp"
#include "item_option.hpp"
#include "item_wire.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

// ─────────────────────────────────────────────────────────────────────────────
//  The post box (PostBoxUI: mail with items / gelders, trade mail asking gelders, the gift box, the level-up
//  rewards). The retail GameServer's Player side (CPostBox at Player +7528, the hPostBox* requests and their
//  OnOk / OnFail) with the MasterServer's cPostStore folded in: a request calls PostStore (the Paula DB's
//  USPM_* procedures, ported) at once and runs its OnOk / OnFail itself.
//  C2S (VERIFIED, client c2s_1230 .. sub_5A9880; the send / receive / delete ones come inside the 0x39F
//  second-password wrapper, which re-posts them): 1230 [u8 list][u8] the list, 1233 [u8][u32 post] its detail,
//  1239 [426-byte form][u8 n][n x {u64 uid, u32 count}] a mail, 1236 [u32 post][str][str] take its items /
//  gelders (the two texts: the title and text of a trade mail's payment), 1242 [u32 post] delete.
//  S2C (VERIFIED, client dispatcher sub_6F0960): 1228 [u16 new][u16 returned] (sub_6ECDC0), 1229 the sync and
//  1231 a list — both in 1920-byte parts [u8 last] (sub_6E06F0) of [u16 new][u16 ret][u16 n] / [u8 list][u8 n]
//  + n x {u8 PostType, row} (row sub_6D3710: [u8 sub][u64 DateSeq][u32 uid][u8 state][u8 user type][u64 FILETIME
//  return][u64 FILETIME sent][str name][str title]); 1234 a detail [u32 post][str text] + (post box, sub_6D67A0)
//  [u32 gelders][u16 n]{u64 seq, item}[u16 m]{u64 seq, u64 uid, u8 type, u16 index, i16 value, u32 mixed} or
//  (gift box, sub_6D56B0 -> sub_6D3850) [u16 n]{u64 seq, item, u32 minutes, u8 flag}; 1237 taken [u32 post][u8 n]
//  {bag record, u64 seq}; 1240 sent [u16 n]{bag record} (+ "PostWarningMsg_6"); 1243 deleted [u32 post][u8 n]
//  {u8 PostType, row}; 1232 / 1235 / 1238 / 1241 / 1244 [u32 code] the errors (sub_9E4EF0 -> sub_B5C800: the
//  game's codes, or >= 101 the procedure's). (S2C 1245 is the couple's partner flag, not the mail.)
//  Not modelled: the zone's "no post box" flag (MapInfo +1128 bit 4 -> 16), timed items ("[Timer]" -> 6), the
//  Master's whisper of new mail (it sends none: a receiver sees new mail at its next sync).
// ─────────────────────────────────────────────────────────────────────────────

namespace {
constexpr size_t kPostListMax = 100;          // CPostBox::InsertPostSeq: 100 posts a list, the oldest goes
constexpr int    kPostChunk   = 1920;         // sub_7C5CB0
constexpr size_t kPostBagCap  = 300;          // sub_6E4290 / sub_6DABE0

void write_cstr(PacketBuffer& p, const std::string& s) { p.write_bytes(s.c_str(), s.size() + 1); }

std::string read_cstr(PacketBuffer& pkt)
{
    std::string s;
    while (pkt.rpos() < pkt.size()) {
        const char ch = static_cast<char>(pkt.read<uint8_t>());
        if (!ch) break;
        s += ch;
    }
    return s;
}

std::string cut(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }

// A local "YYYY-MM-DD HH:MM:SS" as the FILETIME of that local time — the Master's sub_4620F0 (the DB's
// SQL_TIMESTAMP_STRUCT through SystemTimeToFileTime, no zone change); the client sets it against its clock
// (sub_6D3710: the return countdown = sub_813000(return, now) * 1000 ms, now = its local time corrected by
// the server time of S2C 508 — game_session.cpp).
uint64_t filetime_of(const std::string& s)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return 0;
    return local_filetime(y, mo, d, h, mi, se);
}

// The list a post sits in (CPostBox: +112 received, +144 sent, +176 the gift box).
int list_of(const PostRow& r) { return r.post_type == 1 ? 2 : (r.user_type == 1 ? 0 : 1); }

// The length of a fixed-width text of the send form (sub_7A6BC0 / sub_7A6BE0 / sub_725B90): over `width`
// when it has no end inside it.
size_t field_len(const uint8_t* f, size_t width)
{
    size_t n = 0;
    while (n < width && f[n]) ++n;
    return n < width ? n : width + 1;
}
}  // namespace

// ── the cache (CPostBox) ────────────────────────────────────────────────────────────────────────────

// CPostBox::Insert (sub_7C5200) + InsertPostSeq (sub_7C5320): a post already there goes first — unless the
// new row is unread / deleted (then it is refused, "invalid postState"); a list over 100 loses its oldest.
bool GameClientSession::post_insert(const PostRow& r)
{
    auto it = post_box_.find(r.post_uid);
    if (it != post_box_.end()) {
        if (r.state == 0 || r.state == 3) {
            LOG_INFO("CPostBox::Insert - invalid postState, POSTSEQ:%llu, POSTUID:%u, POSTSTATE:%u",
                     static_cast<unsigned long long>(r.date_seq), r.post_uid, static_cast<unsigned>(r.state));
            return false;
        }
        post_box_.erase(it);
    }
    PostEntry e;
    e.row = r;
    post_box_[r.post_uid] = e;
    const int list = list_of(r);
    size_t n = 0;
    uint32_t oldest = 0;
    uint64_t oldest_seq = UINT64_MAX;
    for (const auto& kv : post_box_) {
        if (list_of(kv.second.row) != list) continue;
        ++n;
        if (kv.second.row.date_seq < oldest_seq) { oldest_seq = kv.second.row.date_seq; oldest = kv.first; }
    }
    if (n > kPostListMax) {
        LOG_DEBUG("CPostBox::InsertPostSeq - delete old post, newPostSeq:%llu, delPostSeq:%llu",
                  static_cast<unsigned long long>(r.date_seq), static_cast<unsigned long long>(oldest_seq));
        post_box_.erase(oldest);
    }
    return true;
}

// sub_7C5A00: the oldest DateSeq of a list (0 when empty) — a list request asks for the posts older than it.
uint64_t GameClientSession::post_oldest_seq(int list) const
{
    uint64_t s = 0;
    for (const auto& kv : post_box_)
        if (list_of(kv.second.row) == list && (s == 0 || kv.second.row.date_seq < s)) s = kv.second.row.date_seq;
    return s;
}

// sub_7C5C00: the newest DateSeq of the three lists — a sync asks for the posts newer than it.
uint64_t GameClientSession::post_newest_seq() const
{
    uint64_t s = 0;
    for (const auto& kv : post_box_) s = std::max(s, kv.second.row.date_seq);
    return s;
}

// The Master's post record (sub_468080 / sub_4684F0): [u8 PostType][u8 SubType][u64 DateSeq][u32 PostUID]
// [u8 State][u8 UserType][u64 ReturnDate][u64 SendDate][str CharName (31)][str Title (32)].
void GameClientSession::post_write_row(PacketBuffer& p, const PostRow& r) const
{
    p.write<uint8_t>(r.post_type);
    p.write<uint8_t>(r.sub_type);
    p.write<uint64_t>(r.date_seq);
    p.write<uint32_t>(r.post_uid);
    p.write<uint8_t>(r.state);
    p.write<uint8_t>(r.user_type);
    p.write<uint64_t>(filetime_of(r.return_date));
    p.write<uint64_t>(filetime_of(r.occur_date));
    write_cstr(p, cut(to_client_text(r.char_name), 31));
    write_cstr(p, cut(to_client_text(r.title), 32));
}

// sub_7C5CB0: the payload in parts of 1920 bytes, each [u8 last (its size < 1920)][bytes]. (A payload of an
// exact multiple of 1920 never sends a last part: retail.)
void GameClientSession::post_send_chunks(uint16_t op, const PacketBuffer& payload)
{
    const uint8_t* data = payload.data() + payload.header_size();
    const int total = static_cast<int>(payload.size() - payload.header_size());
    int left = total, at = 0;
    while (left > 0) {
        const int size = (left / kPostChunk) ? kPostChunk : total % kPostChunk;
        PacketBuffer p(static_cast<Opcode>(op), is_paula());
        p.write<uint8_t>(size < kPostChunk ? 1 : 0);
        p.write_bytes(data + at, static_cast<size_t>(size));
        send(p);
        left -= size;
        at += size;
    }
}

void GameClientSession::post_error(uint16_t op, uint32_t code)
{
    PacketBuffer p(static_cast<Opcode>(op), is_paula());
    p.write<uint32_t>(code);
    send(p);
    LOG_DEBUG("GameClientSession %u: post box S2C %u error %u", session_id(), static_cast<unsigned>(op), code);
}

// ── login / sync ────────────────────────────────────────────────────────────────────────────────────

// CMD_CLIENT_LOGIN (and the 156 re-enter): MCmd 3156 -> USPM_Get_PostCount -> 3157 -> S2C 1228 [u16 new]
// [u16 returned] (client sub_6ECDC0: the post box button's counts).
void GameClientSession::post_login()
{
    int fresh = 0, returned = 0;
    if (server_.post_store().Count(char_uid_, fresh, returned) != 0) return;
    PacketBuffer p(static_cast<Opcode>(1228), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(fresh));
    p.write<uint16_t>(static_cast<uint16_t>(returned));
    send(p);
    post_sync_acc_ = 0;
}

// sub_76BF70: the sync every post_sync_minutes (retail dword_ADF858, 15 minutes).
void GameClientSession::post_tick(int elapsed_ms)
{
    post_sync_acc_ += elapsed_ms;
    if (post_sync_acc_ >= server_.post_sync_ms()) post_sync();
}

// sub_76C290 -> MCmd 3139 (USPM_Get_PostSync from the newest DateSeq we hold) -> 3140 -> sub_76C100: the rows
// into the cache, S2C 1229 in parts.
void GameClientSession::post_sync()
{
    post_sync_acc_ = 0;
    int fresh = 0, returned = 0;
    std::vector<PostRow> box, gifts;
    if (server_.post_store().Sync(account_id_, char_uid_, post_newest_seq(), fresh, returned, box, gifts) != 0) return;
    PacketBuffer p(static_cast<Opcode>(1229), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(fresh));
    p.write<uint16_t>(static_cast<uint16_t>(returned));
    p.write<uint16_t>(static_cast<uint16_t>(box.size() + gifts.size()));
    for (const PostRow& r : box)   { post_write_row(p, r); post_insert(r); }
    for (const PostRow& r : gifts) { post_write_row(p, r); post_insert(r); }
    post_send_chunks(1229, p);
}

// ── C2S 1230: a list ────────────────────────────────────────────────────────────────────────────────

// hPostBoxSelectList::Send (sub_79F730): the posts of the list older than its oldest we hold (100 at most)
// -> 3137 OnOk (sub_770520): into the cache, S2C 1231 in parts; 3138 -> S2C 1232.
void GameClientSession::handle_post_list(PacketBuffer& pkt)
{
    try {
        const uint8_t list = pkt.read<uint8_t>();
        if (dead_ || state_ != GameClientSessionState::InWorld) return;
        std::vector<PostRow> rows;
        const int rc = server_.post_store().List(list, account_id_, char_uid_, post_oldest_seq(list),
                                                 static_cast<int>(kPostListMax), rows);
        if (rc != 0) { post_error(1232, static_cast<uint32_t>(rc)); return; }
        PacketBuffer p(static_cast<Opcode>(1231), is_paula());
        p.write<uint8_t>(list);
        p.write<uint8_t>(static_cast<uint8_t>(rows.size()));
        for (const PostRow& r : rows) { post_write_row(p, r); post_insert(r); }
        post_send_chunks(1231, p);
        LOG_DEBUG("GameClientSession %u: post box list %u: %zu post(s)", session_id(), static_cast<unsigned>(list),
                  rows.size());
    } catch (...) {}
}

// ── C2S 1233: a detail ──────────────────────────────────────────────────────────────────────────────

// hPostBoxSelectDetailInfo::Send (sub_79F7F0): a post we hold (else 4) -> USPM_Get_PostDetailInfo (post box) /
// USPM_Get_GiftDetailInfo (gift box) -> 3142 OnOk (sub_7706C0): S2C 1234 + the detail into the cache
// (CPostNormal sub_7C18B0 / CPostGift sub_7C1E10); 3143 -> S2C 1235.
void GameClientSession::handle_post_detail(PacketBuffer& pkt)
{
    try {
        pkt.read<uint8_t>();
        const uint32_t uid = pkt.read<uint32_t>();
        if (dead_ || state_ != GameClientSessionState::InWorld) return;
        auto it = post_box_.find(uid);
        if (it == post_box_.end()) {
            LOG_INFO("hPostBoxSelectDetailInfo::Send - not found post, CHRUID:%u, POSTUID:%u", char_uid_, uid);
            post_error(1235, 4);
            return;
        }
        PostEntry& e = it->second;
        std::string content;
        std::vector<PostItemRow> items;
        std::vector<PostOptionRow> opts;
        int32_t money = 0;
        const bool gift = (e.row.post_type == 1);
        const int rc = gift ? server_.post_store().GiftDetail(uid, account_id_, char_uid_, content, items)
                            : server_.post_store().Detail(uid, account_id_, char_uid_, content, money, items, opts);
        if (rc != 0) { post_error(1235, static_cast<uint32_t>(rc)); return; }
        PacketBuffer p(static_cast<Opcode>(1234), is_paula());
        p.write<uint32_t>(uid);
        write_cstr(p, cut(to_client_text(content), 300));
        if (!gift) p.write<uint32_t>(static_cast<uint32_t>(money));
        p.write<uint16_t>(static_cast<uint16_t>(items.size()));
        for (const PostItemRow& x : items) {
            p.write<uint64_t>(x.item_seq);
            p.write<uint8_t>(x.item_class);
            const uint8_t rare[4] = { 0, x.refine, x.refine_state, x.item_slot };
            write_item_data(p, x.item_class, x.item_index, x.item_uid, static_cast<uint16_t>(x.item_attr), rare);
            if (gift) {                                       // client sub_6D3850: the u32 then the u8
                p.write<uint32_t>(static_cast<uint32_t>(x.add_minute));
                p.write<uint8_t>(x.item_flag);
            }
        }
        if (!gift) {
            p.write<uint16_t>(static_cast<uint16_t>(opts.size()));
            for (const PostOptionRow& o : opts) {
                p.write<uint64_t>(o.item_seq);
                p.write<uint64_t>(o.item_uid);
                p.write<uint8_t>(o.type);
                p.write<int16_t>(o.index);
                p.write<int16_t>(o.value);
                p.write<int32_t>(o.mixed);
            }
        }
        send(p);
        e.detail = true;
        e.money  = money;
        e.items  = items;
        e.opts   = opts;
        // CPostNormal: no item and no gelders (and not a payment) -> nothing left (+20); CPostGift: no item
        e.done = gift ? items.empty() : (e.row.sub_type != 3 && items.empty() && money == 0);
        LOG_DEBUG("GameClientSession %u: post %u detail: %zu item(s), %d gelders", session_id(), uid, items.size(), money);
    } catch (...) {}
}

// ── C2S 1239: send ──────────────────────────────────────────────────────────────────────────────────

// hPostBoxSendPost::Send (sub_79F8E0) -> USPM_Send_PostItem -> 3145 OnOk (sub_770810): the bag records, S2C
// 1240, a sync; 3146 OnFail (sub_770EC0): S2C 1241 (the gelders and items stay).
void GameClientSession::handle_post_send(PacketBuffer& pkt)
{
    try {
        uint8_t f[426];
        for (uint8_t& b : f) b = pkt.read<uint8_t>();
        const uint8_t n = pkt.read<uint8_t>();
        struct Att { uint64_t uid; uint32_t count; };
        std::vector<Att> att;
        for (uint8_t i = 0; i < n && i < 16; ++i) {
            Att a;
            a.uid   = pkt.read<uint64_t>();
            a.count = pkt.read<uint32_t>();
            att.push_back(a);
        }
        // Hardening (not retail): one entry per stack — the same uid attached twice made two entries of the post (two
        // stacks of one id on the receiver's side); their counts add up and are checked against the stack below.
        for (size_t i = 0; i < att.size(); ++i)
            for (size_t j = att.size() - 1; j > i; --j)
                if (att[j].uid == att[i].uid) {
                    const uint64_t sum = static_cast<uint64_t>(att[i].count) + att[j].count;
                    att[i].count = (sum > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(sum);
                    att.erase(att.begin() + static_cast<std::ptrdiff_t>(j));
                }
        if (dead_ || state_ != GameClientSessionState::InWorld) return;
        const uint8_t type = f[1];
        if (field_len(f + 2, 33) > 33)   { post_error(1241, 5); return; }
        if (field_len(f + 35, 301) > 301) { post_error(1241, 8); return; }
        if (field_len(f + 394, 32) > 32)  { post_error(1241, 9); return; }
        if (type == 4 || type == 5 || type == 6) { post_error(1241, 13); return; }
        if (n >= 8) {
            LOG_INFO("hPostBoxSendPost::Send - too many item, CHRUID:%u, ItemOpCount:%u", char_uid_, static_cast<unsigned>(n));
            post_error(1241, 10);
            return;
        }
        const std::string to(reinterpret_cast<const char*>(f + 2), field_len(f + 2, 33));
        const std::string body(reinterpret_cast<const char*>(f + 35), field_len(f + 35, 301));
        const std::string title(reinterpret_cast<const char*>(f + 394), field_len(f + 394, 32));
        int32_t money = 0;
        std::memcpy(&money, f + 340, 4);
        const PostSetupDb& cfg = server_.post_setup_db();
        const float rate = cfg.fee_rate(type);
        int64_t fee = 0;                                      // (int64: count * Value can pass 2^31)

        std::vector<BagRecord> recs;                          // the sender's bag (sub_6DBF00)
        std::vector<PostItemOp> from, to_post;
        std::map<uint64_t, int> left;                         // what each stack keeps so far
        for (const Att& a : att) {
            BagItem* b = bag_find_uid(a.uid);
            bool worn = false;                                // (the retail bag, +1716, holds no worn item)
            for (uint64_t w : equip_) if (w && w == a.uid) worn = true;
            if (!b || worn) {
                LOG_INFO("hPostBoxSendPost::Send - not found itemUid, CHRUID:%u, ITMUID:%llu", char_uid_,
                         static_cast<unsigned long long>(a.uid));
                post_error(1241, 5);
                return;
            }
            const ItemDef* d = server_.item_db().get(b->item_id);
            if (!d) { post_error(1241, 12); return; }
            if (d->options & 1024) {
                LOG_INFO("hPostBoxSendPost::Send - invalid attached item[NotExchangeable], CHRUID:%u, ITMUID:%llu",
                         char_uid_, static_cast<unsigned long long>(a.uid));
                post_error(1241, 6);
                return;
            }
            if (d->options & 512) {
                LOG_INFO("hPostBoxSendPost::Send - invalid attached item[NotSellable], CHRUID:%u, ITMUID:%llu",
                         char_uid_, static_cast<unsigned long long>(a.uid));
                post_error(1241, 6);
                return;
            }
            if (!left.count(a.uid)) left[a.uid] = d->item_count(b->count);
            // (unsigned: the count is the client's u32 — as an int, 0x80000000.. passed and raised what the stack keeps)
            if (a.count == 0 || a.count > static_cast<uint32_t>(left[a.uid])) {
                LOG_INFO("hPostBoxSendPost::Send - invalid attached item, CHRUID:%u, ITMUID:%llu", char_uid_,
                         static_cast<unsigned long long>(a.uid));
                post_error(1241, 10);
                return;
            }
            if (type == 1) {
                const int64_t v23 = static_cast<int64_t>(a.count) * d->value;
                fee += static_cast<int64_t>(static_cast<double>(v23) * static_cast<double>(rate));
            }
            left[a.uid] -= static_cast<int>(a.count);
            const bool whole = (left[a.uid] == 0);
            BagRecord r;
            r.kind    = b->kind;
            r.item_id = b->item_id;
            r.uid     = b->uid;
            std::memcpy(r.rare, b->rare, 4);
            if (whole) { r.type = 3; r.count = b->count; }
            else       { r.type = 1; r.count = static_cast<uint16_t>(left[a.uid]); }
            recs.push_back(r);
            PostItemOp out;                                   // the sender's side of the stream
            out.uid   = b->uid;
            out.index = b->item_id;
            out.cls   = b->kind;
            out.flag  = whole ? 0 : 2;
            out.count = whole ? b->count : static_cast<uint16_t>(left[a.uid]);
            from.push_back(out);
            PostItemOp in;                                    // the post's side
            in.uid   = b->uid;
            in.index = b->item_id;
            in.cls   = b->kind;
            in.flag  = 1;
            in.count = static_cast<uint16_t>(a.count);
            to_post.push_back(in);
        }
        fee += static_cast<int64_t>(static_cast<double>(money) * static_cast<double>(rate));
        if (fee <= cfg.get(5)) fee = cfg.get(5);
        int64_t after = money_;
        if (type == 1) {
            after -= money;
            if (after < 0) {
                LOG_INFO("hPostBoxSendPost::Send - not enough money, CHRUID:%u, curMoney:%d, inMoney:%d", char_uid_,
                         money_, money);
                post_error(1241, 11);
                return;
            }
            after -= fee;
            if (after < 0) {
                LOG_INFO("hPostBoxSendPost::Send - not enough money, CHRUID:%u, curMoney:%d, inMoney:%d, fee:%lld",
                         char_uid_, money_, money, static_cast<long long>(fee));
                post_error(1241, 11);
                return;
            }
        } else if (type == 0) {
            fee = 0;
            if (money > 0) {
                LOG_INFO("hPostBoxSendPost::Send - invalid data, CHRUID:%u", char_uid_);
                post_error(1241, 13);
                return;
            }
        }
        if (type != 0 && type != 1 && money < static_cast<int32_t>(cfg.get(5))) { post_error(1241, 28); return; }

        PostStore::SendArgs a;
        a.sub_type   = type;
        a.title      = from_client_text(title);
        a.content    = from_client_text(body);
        a.tax        = (fee > 0x7FFFFFFF) ? 0x7FFFFFFF : static_cast<int32_t>(fee);
        a.money      = money;
        a.from_user  = account_id_;
        a.from_char  = char_uid_;
        a.from_name  = char_name_;
        a.from_level = level_;
        a.to_name    = from_client_text(to);
        a.from_money_final = (type == 1) ? static_cast<int32_t>(after) : -1;   // (0..money_)
        uint32_t post_uid = 0;
        const int rc = server_.post_store().Send(a, from, to_post, post_uid);
        if (rc != 0) {                                        // OnFail: S2C 1241 [code]
            post_error(1241, static_cast<uint32_t>(rc));
            return;
        }
        // OnOk: the gelders, the bag records (S2C 1240), a sync
        if (type == 1 && after != money_) { money_ = static_cast<int32_t>(after); send_money(); }
        for (const BagRecord& r : recs) bag_apply(r);
        PacketBuffer p(static_cast<Opcode>(1240), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(recs.size()));
        for (const BagRecord& r : recs) write_bag_record(p, r);
        send(p);
        recompute_stats();
        LOG_INFO("GameClientSession %u: post %u sent to %s (type %u, %zu item(s), %d gelders, fee %d)", session_id(),
                 post_uid, a.to_name.c_str(), static_cast<unsigned>(type), recs.size(), money, a.tax);
        post_sync();
    } catch (...) {}
}

// ── C2S 1236: take a post's items / gelders ─────────────────────────────────────────────────────────

// hPostBoxRecvPost::Send (sub_7A0270): MakeRecvItemInfo (CPostNormal sub_7C1A80 / CPostGift sub_7C1F20) ->
// USPM_Recv_PostItem / USPM_Recv_GiftItem -> 3148 OnOk (sub_771000): the items into the bag (post vtbl+20
// sub_7C1590), S2C 1237, a sync; 3149 OnFail (sub_7716A0): S2C 1238.
void GameClientSession::handle_post_recv(PacketBuffer& pkt)
{
    try {
        const uint32_t uid = pkt.read<uint32_t>();
        const std::string sys_title   = read_cstr(pkt);
        const std::string sys_content = read_cstr(pkt);
        if (dead_ || state_ != GameClientSessionState::InWorld) return;
        auto it = post_box_.find(uid);
        if (it == post_box_.end()) {
            LOG_INFO("PostBoxRecvPost::Send - not found post, CHRUID:%u, POSTUID:%u", char_uid_, uid);
            post_error(1238, 4);
            return;
        }
        PostEntry& e = it->second;
        const PostRow& r = e.row;
        // sub_7C16B0: a mail that is not a normal one, held by its receiver, must still be in its time
        if (r.sub_type != 0 && r.user_type != 0 && filetime_of(r.return_date) < filetime_of(PostStore::Now())) {
            LOG_INFO("PostBoxRecvPost::Send - over the day, CHRUID:%u, POSTUID:%u", char_uid_, uid);
            post_error(1238, 17);
            return;
        }
        if (r.sub_type == 0 || r.sub_type == 6) {
            LOG_INFO("PostBoxRecvPost::Send - invalid post Type, CHRUID:%u, POSTTYPE:%u", char_uid_,
                     static_cast<unsigned>(r.sub_type));
            post_error(1238, 13);
            return;
        }
        if (r.state == 1 || r.state == 3) {
            LOG_INFO("PostBoxRecvPost::Send - invalid post State, CHRUID:%u, POSTUID:%u, POSTSTATE:%u", char_uid_, uid,
                     static_cast<unsigned>(r.state));
            post_error(1238, 16);
            return;
        }
        const bool gift = (r.post_type == 1);
        // MakeRecvItemInfo: each item's record against the bag (a simulated one: two items of one id add up —
        // the retail computes each against the bag as it was), the weight / room (15)
        if (gift && kPostBagCap - bag_.size() < e.items.size()) { post_error(1238, 15); return; }
        std::vector<BagRecord> recs;
        std::vector<PostItemOp> ops;
        std::vector<PostOptionRow> gift_opts;
        std::map<uint64_t, uint16_t> stack_now;               // a joined stack's count so far
        std::set<uint32_t> new_stack;                         // the stackable ids a new stack is made of
        int32_t weight = bag_weight_;
        size_t items_now = bag_.size();
        for (const PostItemRow& x : e.items) {
            const ItemDef* d = server_.item_db().get(x.item_index);
            if (!d) { post_error(1238, 15); return; }
            const int cnt = (x.item_class == 1) ? x.item_attr : 1;
            BagItem* stack = nullptr;
            if (d->stackable())
                for (BagItem& b : bag_) if (b.item_id == d->id) { stack = &b; break; }
            // Hardening (not retail): a second entry of a stackable id the bag has no stack of made a second new stack
            // of it (each entry is computed against the bag as it was) — the precondition of the uid-less dupes. Such
            // a mail is refused (the send side merges one stack's entries; no level-up gift repeats an id).
            if (d->stackable() && !stack && !new_stack.insert(d->id).second) {
                LOG_INFO("PostBoxRecvPost::Send - item %u twice as a new stack, CHRUID:%u, POSTUID:%u", d->id, char_uid_, uid);
                post_error(1238, 15);
                return;
            }
            const int add = d->weight * cnt;
            if (weight + add > st_maxwt_ || (!stack && items_now >= kPostBagCap)) {
                LOG_INFO("CPostNormal::OnMakeRecvItemInfo - overflow inven, CHRUID:%u", char_uid_);
                post_error(1238, 15);
                return;
            }
            weight += add;
            BagRecord br;
            br.kind    = static_cast<uint8_t>(d->cls);
            br.item_id = d->id;
            PostItemOp op;
            op.index    = d->id;
            op.cls      = static_cast<uint8_t>(d->cls);
            op.item_seq = x.item_seq;
            if (stack) {                                      // joins the stack: record 1, flag 2
                if (!stack_now.count(stack->uid)) stack_now[stack->uid] = stack->count;
                stack_now[stack->uid] = static_cast<uint16_t>(stack_now[stack->uid] + x.item_attr);
                br.type  = 1;
                br.uid   = stack->uid;
                br.count = stack_now[stack->uid];
                op.flag  = 2;
                op.uid   = stack->uid;
                op.count = br.count;
            } else {
                ++items_now;
                br.count = static_cast<uint16_t>(x.item_attr);
                op.flag  = 1;
                op.count = static_cast<uint16_t>(x.item_attr);
                if (d->cls == 2 && !gift && x.item_uid) {     // a mailed rare item keeps its row: record 2
                    br.type = 2;
                    br.uid  = x.item_uid;
                    br.rare[1] = x.refine; br.rare[2] = x.refine_state; br.rare[3] = x.item_slot;
                    for (const PostOptionRow& o : e.opts)
                        if (o.item_seq == x.item_seq) br.opts.push_back({ o.type, o.index, o.value, o.mixed });
                    op.uid = x.item_uid;
                } else {                                      // a new row: record 4 (its uid from the DB)
                    br.type = 4;
                    if (d->cls == 2) {
                        br.rare[3] = static_cast<uint8_t>(d->compound_slot);
                        op.slot = br.rare[3];
                        base_item_options(*d, br.opts);       // CPostGift: the item's own options (sub_6E0090)
                        for (const ItemOption& o : br.opts) {
                            PostOptionRow pr;
                            pr.item_seq = x.item_seq;
                            pr.type = static_cast<uint8_t>(o.type);
                            pr.index = o.id;
                            pr.value = o.value;
                            pr.mixed = o.extra;
                            gift_opts.push_back(pr);
                        }
                    }
                }
            }
            recs.push_back(br);
            ops.push_back(op);
        }
        // the gelders (taken now; a returned gift's come back, a trade mail is paid)
        const int32_t money = e.money;
        int32_t after = money_;
        if (r.state == 2) { if (r.sub_type != 2) after += money; }
        else if (r.sub_type == 2) after -= money;
        else after += money;
        if (after < 0) { post_error(1238, 11); return; }

        PostStore::RecvArgs a;
        a.post_uid    = uid;
        a.state       = r.state;
        a.to_user     = account_id_;
        a.to_char     = char_uid_;
        a.to_level    = level_;
        a.to_name     = char_name_;
        a.sys_title   = from_client_text(sys_title);
        a.sys_content = from_client_text(sys_content);
        a.money       = money;
        a.to_money_final = after;
        const int rc = gift ? server_.post_store().RecvGift(a, ops, gift_opts)
                            : server_.post_store().Recv(a, ops);
        if (rc != 0) { post_error(1238, static_cast<uint32_t>(rc)); return; }

        // OnOk (post vtbl+20 sub_7C1590): the records with the rows' uids into the bag, S2C 1237, a sync
        if (after != money_) { money_ = after; send_money(); }
        PacketBuffer p(static_cast<Opcode>(1237), is_paula());
        p.write<uint32_t>(uid);
        p.write<uint8_t>(static_cast<uint8_t>(recs.size()));
        for (size_t i = 0; i < recs.size(); ++i) {
            BagRecord& br = recs[i];
            if (br.type == 4) br.uid = ops[i].new_uid;
            if (br.type == 1) {
                bag_apply(br);
            } else {
                BagItem b;
                b.uid     = br.uid;
                b.item_id = br.item_id;
                b.count   = br.count;
                b.kind    = br.kind;
                std::memcpy(b.rare, br.rare, 4);
                b.opts    = br.opts;
                bag_.push_back(b);
                if (const ItemDef* d = server_.item_db().get(br.item_id)) bag_weight_ += d->weight * d->item_count(br.count);
            }
            write_bag_record(p, br);
            p.write<uint64_t>(ops[i].item_seq);
        }
        send(p);
        e.done = true;
        recompute_stats();
        LOG_INFO("GameClientSession %u: post %u taken: %zu item(s), gelders %d -> %d", session_id(), uid, recs.size(),
                 money, money_);
        post_sync();
    } catch (...) {}
}

// ── C2S 1242: delete ────────────────────────────────────────────────────────────────────────────────

// hPostBoxDeletePost::Send (sub_7A06D0): the post leaves the cache first, then USPM_Delete_Post (its DateSeq, the
// oldest of its list we still hold) -> 3151 OnOk (sub_771760): S2C 1243 [u32 post] + the next older row (into
// the cache too); 3152 -> S2C 1244.
void GameClientSession::handle_post_delete(PacketBuffer& pkt)
{
    try {
        const uint32_t uid = pkt.read<uint32_t>();
        if (dead_ || state_ != GameClientSessionState::InWorld) return;
        auto it = post_box_.find(uid);
        if (it == post_box_.end()) {
            LOG_INFO("hPostBoxDeletePost::Send - not found postItem, CHRUID:%u, POSTUID:%u", char_uid_, uid);
            post_error(1244, 4);
            return;
        }
        const PostRow r = it->second.row;
        post_box_.erase(it);
        const int list = (r.post_type != 0) ? 2 : (r.user_type != 1 ? 1 : 0);
        std::vector<PostRow> next;
        const int rc = server_.post_store().Delete(uid, account_id_, char_uid_, r.date_seq, post_oldest_seq(list), next);
        if (rc != 0) { post_error(1244, static_cast<uint32_t>(rc)); return; }
        PacketBuffer p(static_cast<Opcode>(1243), is_paula());
        p.write<uint32_t>(uid);
        p.write<uint8_t>(static_cast<uint8_t>(next.size()));
        for (const PostRow& x : next) { post_write_row(p, x); post_insert(x); }
        send(p);
        LOG_DEBUG("GameClientSession %u: post %u deleted (%zu next)", session_id(), uid, next.size());
    } catch (...) {}
}

// ── the level-up gifts ──────────────────────────────────────────────────────────────────────────────

// sub_76BE80 (Player level-up sub_75D390 -> each PostBoxLevelUpGiftInfo row of the level whose ChrType has our
// type) -> MCmd 3153 -> USPM_Send_GMGiftItem -> 3154 -> a sync. sub_79F6D0: the current type (Player +436, 1..24)
// through dword_9C8F18 = 1 << (type - 1) (read from the EXE); another type matches nothing.
void GameClientSession::post_level_up(int level)
{
    const uint32_t race = (type_ >= 1 && type_ <= 24) ? (1u << (type_ - 1)) : 0;
    bool sent = false;
    for (const LevelUpGiftDb::Gift* g : server_.levelup_gift_db().of_level(level)) {
        if (!(g->chr_type & race)) continue;
        std::vector<PostStore::GiftItem> items;
        for (const LevelUpGiftDb::Item& it : g->items) {
            const ItemDef* d = server_.item_db().get(it.id);
            if (!d) continue;
            PostStore::GiftItem gi;
            gi.index = it.id;
            gi.cls   = static_cast<uint8_t>(d->cls);
            gi.attr  = static_cast<int16_t>(it.count);
            items.push_back(gi);
        }
        const int rc = server_.post_store().SendGMGift(g->title, g->message, char_uid_, g->sender, items);
        LOG_INFO("GameClientSession %u: level %d gift %d (%zu item(s)) -> %d", session_id(), level, g->id,
                 items.size(), rc);
        if (rc == 0) sent = true;
    }
    if (sent) post_sync();
}
