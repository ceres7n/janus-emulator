#include "friend_server.hpp"
#include "game_server.hpp"
#include "game_session.hpp"
#include "client_text.hpp"
#include "logger.hpp"

namespace {
void write_str(PacketBuffer& p, const std::string& s) { p.write_bytes(s.c_str(), s.size() + 1); }

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

// usp_select_friend_info: the type a character shows (its job state's model)
constexpr const char* kShownType =
    "CASE WHEN c.job_state = 2 THEN c.type_2 WHEN c.job_state = 3 THEN c.type_3 ELSE c.type END";

// usp_friend_group_create (the Paula English names)
const char* const kGroups[5] = { "Best Friends", "Soulmates", "Companions", "Relatives", "Good Fellas" };

constexpr size_t kFriendMax = 60;
constexpr int    kSmsLimit  = 5;          // SystemServer.ini FriendOfflineSMSLimitCount (sub_461820)
// Hardening (not retail): the longest note text we relay or store, in client (CP949) bytes = the width of
// friend_sms.content (sql/014); an unbounded one (up to the 65 KB frame) wrapped the S2C frame length.
constexpr size_t kNoteMax   = 255;
}  // namespace

std::string FriendServer::key(const std::string& name)
{
    std::string k = name;
    for (char& c : k) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return k;
}

FriendServer::Client* FriendServer::by_sid(uint32_t sid)
{
    auto it = clients_.find(sid);
    return it == clients_.end() ? nullptr : &it->second;
}

FriendServer::Client* FriendServer::by_uid(uint32_t uid)
{
    for (auto& kv : clients_) if (kv.second.uid == uid) return &kv.second;
    return nullptr;
}

FriendServer::Client* FriendServer::by_name(const std::string& name)
{
    const std::string k = key(name);
    for (auto& kv : clients_) if (key(kv.second.name) == k) return &kv.second;
    return nullptr;
}

bool FriendServer::has(const Client& c, uint32_t uid)
{
    for (const Entry& e : c.friends) if (e.uid == uid) return true;
    return false;
}

void FriendServer::send(const Client& c, uint16_t op, const PacketBuffer* body)
{
    PacketBuffer p(static_cast<Opcode>(op), c.paula);
    if (body) p.write_bytes(body->data() + body->header_size(), body->size() - body->header_size());
    srv_.send_to_system(c.sid, p);
}

void FriendServer::send_raw(const Client& c, PacketBuffer& p) { srv_.send_to_system(c.sid, p); }

// ── FriendLoginCmd (sub_468520 .. sub_468D40 -> sub_468580) ─────────────────────────────────────────
// The names that dropped us while we were away (usp_select_friend_info_deleted, then removed), the five groups
// (usp_select_friend_group_info; none -> usp_friend_group_create), the friends (usp_select_friend_info), the notes
// (usp_select_friend_sms, then removed) -> S2C 8510 (8537 with no friend). A friend online whose list holds us is
// shown online and told (FriendShipOnActiveCmd sub_471160: S2C 8511 [u64 our uid]).
void FriendServer::login(uint32_t sys_sid, uint32_t char_uid, bool paula)
{
    Database& db = srv_.db();
    auto me_rows = db.query("SELECT c.name, %s AS t FROM characters c WHERE c.uid = %u", kShownType, char_uid);
    if (me_rows.empty()) {
        LOG_INFO("FRIEND SERVER    LOGIN FAIL - chr_uid : %u", char_uid);
        return;
    }
    Client cl;
    cl.sid   = sys_sid;
    cl.uid   = char_uid;
    cl.name  = me_rows[0].at("name");
    cl.type  = static_cast<uint16_t>(std::stoi(me_rows[0].at("t")));
    cl.paula = paula;

    std::vector<std::string> dropped;
    for (const DBRow& r : db.query("SELECT c.name FROM friend_info f JOIN characters c ON c.uid = f.friendid "
                                   "WHERE f.ownerid = %u AND f.deleted = 1", char_uid))
        dropped.push_back(r.at("name"));
    if (!dropped.empty()) db.execute("DELETE FROM friend_info WHERE ownerid = %u AND deleted = 1", char_uid);

    auto groups = db.query("SELECT groupNum, groupName FROM friend_group_info WHERE ownerid = %u", char_uid);
    if (groups.empty()) {
        for (int i = 0; i < 5; ++i)
            db.execute("INSERT INTO friend_group_info (ownerid, groupNum, groupName) VALUES (%u, %d, '%s')", char_uid, i,
                       kGroups[i]);
        groups = db.query("SELECT groupNum, groupName FROM friend_group_info WHERE ownerid = %u", char_uid);
    }
    for (const DBRow& r : groups) {
        const int g = std::stoi(r.at("groupNum"));
        if (g >= 0 && g < 5) cl.groups[g] = r.at("groupName");
    }
    for (const DBRow& r : db.query("SELECT f.friendid, f.groupNum, %s AS t, c.name FROM friend_info f JOIN characters c "
                                   "ON c.uid = f.friendid WHERE f.ownerid = %u AND f.deleted = 0", kShownType, char_uid)) {
        Entry e;
        e.uid   = static_cast<uint32_t>(std::stoul(r.at("friendid")));
        e.group = static_cast<uint8_t>(std::stoi(r.at("groupNum")));
        e.type  = static_cast<uint16_t>(std::stoi(r.at("t")));
        e.name  = r.at("name");
        cl.friends.push_back(e);
    }
    struct Note { uint32_t sender; std::string date, text, name; uint32_t kind; };
    std::vector<Note> notes;
    for (const DBRow& r : db.query("SELECT s.sender, s.dtime, s.content, c.name, s.sms_kind FROM friend_sms s JOIN "
                                   "characters c ON c.uid = s.sender WHERE s.ownerid = %u ORDER BY s.sms_kind DESC "
                                   "LIMIT %d", char_uid, 2 * kSmsLimit))   // (FriendLoginCmd case 20: 2 x the limit)
        notes.push_back({ static_cast<uint32_t>(std::stoul(r.at("sender"))), r.at("dtime"), r.at("content"), r.at("name"),
                          static_cast<uint32_t>(std::stoul(r.at("sms_kind"))) });
    if (!notes.empty()) db.execute("DELETE FROM friend_sms WHERE ownerid = %u", char_uid);

    std::lock_guard<std::recursive_mutex> lk(mx_);
    clients_.erase(sys_sid);
    clients_[sys_sid] = cl;
    Client& me = clients_[sys_sid];
    const bool any = !me.friends.empty();
    PacketBuffer p(static_cast<Opcode>(any ? 8510 : 8537), paula);
    p.write<uint32_t>(static_cast<uint32_t>(dropped.size()));
    for (const std::string& n : dropped) write_str(p, n);
    p.write<uint32_t>(5);
    for (int i = 0; i < 5; ++i) {
        p.write<uint32_t>(static_cast<uint32_t>(i));
        write_str(p, to_client_text(me.groups[i]));
    }
    if (any) {
        p.write<uint32_t>(static_cast<uint32_t>(me.friends.size()));
        for (const Entry& e : me.friends) {
            Client* f = by_uid(e.uid);
            uint8_t online = 0;
            if (f && f->logged && has(*f, me.uid)) {          // FriendShipOnActiveCmd on the friend
                online = 1;
                PacketBuffer n(static_cast<Opcode>(8511), f->paula);
                n.write<uint64_t>(me.uid);
                send_raw(*f, n);
            }
            p.write<uint64_t>(e.uid);
            p.write<uint8_t>(e.group);
            write_str(p, e.name);
            p.write<uint8_t>(online);
            p.write<uint16_t>(e.type);
        }
    }
    p.write<uint32_t>(static_cast<uint32_t>(notes.size()));
    for (const Note& n : notes) {
        p.write<uint64_t>(n.sender);
        write_str(p, n.date);
        write_str(p, to_client_text(n.text));
        write_str(p, n.name);
        p.write<uint32_t>(n.kind);
    }
    send_raw(me, p);
    me.logged = true;
    LOG_INFO("FRIEND SERVER    LOGIN SUCCESS- chr_uid : %u (%zu friend(s), %zu dropped, %zu note(s))", char_uid,
             me.friends.size(), dropped.size(), notes.size());
}

// FriendLogOutCmd / FriendShipDeActiveCmd: each friend online whose list holds us is told (FriendShipOnDeActiveCmd
// sub_4714A0: S2C 8512 [u64 our uid]).
void FriendServer::logout(uint32_t sys_sid)
{
    std::lock_guard<std::recursive_mutex> lk(mx_);
    Client* me = by_sid(sys_sid);
    if (!me) return;
    const uint32_t uid = me->uid;
    if (me->logged)
        for (const Entry& e : me->friends) {
            Client* f = by_uid(e.uid);
            if (!f || !f->logged || !has(*f, uid)) continue;
            PacketBuffer n(static_cast<Opcode>(8512), f->paula);
            n.write<uint64_t>(uid);
            send_raw(*f, n);
        }
    if (!me->pending.empty())                                // an open request goes with us
        if (Client* a = by_name(me->pending)) a->busy = false;
    clients_.erase(sys_sid);
    LOG_INFO("FRIEND SERVER    LOGOUT SUCCESS- chr_uid : %u", uid);
}

// FriendServer::OnCmd (sub_46DC80)
void FriendServer::command(uint32_t sys_sid, PacketBuffer& pkt, uint16_t op, bool paula)
{
    (void)paula;
    if (op == 8502) { logout(sys_sid); return; }
    std::lock_guard<std::recursive_mutex> lk(mx_);
    Client* me = by_sid(sys_sid);
    if (!me) return;
    switch (op) {
    case 8503: ask(*me, read_cstr(pkt)); return;
    case 8504: drop(*me, read_cstr(pkt)); return;
    case 8505: drop_offline(*me, read_cstr(pkt)); return;
    case 8506: { const uint64_t a = pkt.read<uint64_t>(); pkt.read<uint8_t>(); read_cstr(pkt);
                 yes(*me, static_cast<uint32_t>(a)); return; }
    case 8507: no(*me, static_cast<uint32_t>(pkt.read<uint64_t>())); return;
    case 8519: cancel(*me, read_cstr(pkt)); return;
    case 8538: { const uint64_t u = pkt.read<uint64_t>(); const uint8_t old = pkt.read<uint8_t>();
                 const uint32_t g = pkt.read<uint32_t>(); move(*me, static_cast<uint32_t>(u), old, g); return; }
    case 8541: { const uint32_t g = pkt.read<uint32_t>(); rename(*me, g, read_cstr(pkt)); return; }
    case 8543: me->accept = pkt.read<uint16_t>(); return;   // sub_46D630: +186
    case 8575: cannot(*me, static_cast<uint32_t>(pkt.read<uint64_t>())); return;
    case 8556: { const uint64_t to = pkt.read<uint64_t>(); note(*me, static_cast<uint32_t>(to), read_cstr(pkt)); return; }
    case 8561: { const uint64_t to = pkt.read<uint64_t>(); note_offline(*me, static_cast<uint32_t>(to), read_cstr(pkt)); return; }
    case 8573: note_refused(*me, static_cast<uint32_t>(pkt.read<uint64_t>())); return;
    case 8551: position(*me, static_cast<uint32_t>(pkt.read<uint64_t>())); return;
    case 8586: change_zone_to(*me, static_cast<uint32_t>(pkt.read<uint64_t>())); return;
    default:
        LOG_DEBUG("FriendServer: C2S %u not done (%zu bytes, chr %u)", op, pkt.size(), me->uid);
        return;
    }
}

// 8503 (sub_46D0B0): ourselves -> 8525; 60 friends -> 8542, the asked one's 60 -> 8550; then FriendRequestCmd (by uid
// when it is online, else by name: no such character -> 8509, not online -> 8513), the "office" checks
// (FriendOfficeSelfCmd sub_46B520 / FriendOfficeCmd sub_46A660 -> agency 181 -> the game server's
// FriendObserveStateAndLevelProcessor sub_828A70: our Player state and then the asked one's 0 / 8 / 15, else — or not
// found — 8508 [17]; the level at least FriendAskLimitLevel / FriendResponseLimitLevel, SystemServer.ini 0: never
// 8508 [18]), FriendAddCmd (sub_45CE80): takes none -> 8544, busy -> 8526, not logged in -> 8508 [15]; else both busy,
// S2C 8516 [u64 our uid][str our name] to it (its +152 = our name) and 8548 to us.
void FriendServer::ask(Client& me, const std::string& name)
{
    Client* t = by_name(name);
    const uint32_t tu = t ? t->uid : 0;
    if (tu == me.uid) { send(me, 8525); return; }
    if (me.friends.size() >= kFriendMax) { send(me, 8542); return; }
    if (t && t->friends.size() >= kFriendMax) { send(me, 8550); return; }
    if (!t) {
        const bool exists = !srv_.db().query("SELECT uid FROM characters WHERE name = '%s' AND deleted = 0",
                                             srv_.db().escape(from_client_text(name)).c_str()).empty();
        send(me, exists ? 8513 : 8509);
        return;
    }
    {
        auto normal = [&](uint32_t uid) {
            std::shared_ptr<GameClientSession> s = srv_.session_of_char(uid);
            return s && s->office_state_ok();
        };
        if (!normal(me.uid) || !normal(tu)) {
            PacketBuffer b(static_cast<Opcode>(8508), me.paula);
            b.write<uint8_t>(17);
            send_raw(me, b);
            return;
        }
    }
    if (!t->accept) { send(me, 8544); return; }
    if (t->busy) { send(me, 8526); return; }
    if (!t->logged) {
        PacketBuffer b(static_cast<Opcode>(8508), me.paula);
        b.write<uint8_t>(15);
        send_raw(me, b);
        return;
    }
    me.busy = t->busy = true;
    PacketBuffer q(static_cast<Opcode>(8516), t->paula);
    q.write<uint64_t>(me.uid);
    write_str(q, me.name);
    send_raw(*t, q);
    t->pending = me.name;
    send(me, 8548);
    LOG_INFO("FriendServer: %s asks %s", me.name.c_str(), t->name.c_str());
}

// 8506 -> FriendAddYesCmd (sub_45E4E0): the asker gone -> 8545; already friends -> 8529 (+ 8584 when only one list
// holds the other); else usp_insert_friend_info (both rows) -> OnResultFromDB (sub_45EAF0 / sub_45E7F0): S2C 8518
// [u64 asker][u8 0][str][u16 type] to us, 8514 [u64 us][u8 0][str][u8 1][u16 type] to the asker; a DB error -> 8583.
void FriendServer::yes(Client& me, uint32_t asker)
{
    Client* a = by_uid(asker);
    auto clear = [&]() { me.busy = false; me.pending.clear(); if (a) a->busy = false; };
    if (!a) { send(me, 8545); clear(); return; }
    if (!me.logged || !a->logged) { clear(); return; }
    if (has(me, asker)) {
        send(me, 8529);
        if (!has(*a, me.uid)) send(me, 8584);
        clear();
        return;
    }
    Database& db = srv_.db();
    int err = 0;
    {
        DBTransaction tx(db);
        if (!db.query("SELECT ownerid FROM friend_info WHERE ownerid = %u AND friendid = %u", me.uid, asker).empty()) {
            err = 103;
        } else {
            db.execute("INSERT INTO friend_info (ownerid, friendid, groupNum, deleted) VALUES (%u, %u, 0, 0)", me.uid, asker);
            db.execute("INSERT INTO friend_info (ownerid, friendid, groupNum, deleted) VALUES (%u, %u, 0, 0)", asker, me.uid);
            tx.commit();
        }
    }
    if (err) {
        LOG_INFO("FriendAddYesCmd::OnResultFromDB:eFriendQuery_AddFriend error! (error:%d)", err);
        send(me, 8583);
        send(me, 8583);
        clear();
        return;
    }
    me.friends.push_back({ asker, 0, a->name, a->type });
    PacketBuffer m(static_cast<Opcode>(8518), me.paula);
    m.write<uint64_t>(asker);
    m.write<uint8_t>(0);
    write_str(m, a->name);
    m.write<uint16_t>(a->type);
    send_raw(me, m);
    a->friends.push_back({ me.uid, 0, me.name, me.type });
    PacketBuffer o(static_cast<Opcode>(8514), a->paula);
    o.write<uint64_t>(me.uid);
    o.write<uint8_t>(0);
    write_str(o, me.name);
    o.write<uint8_t>(1);
    o.write<uint16_t>(me.type);
    send_raw(*a, o);
    clear();
    LOG_INFO("FriendServer: %s and %s are friends", me.name.c_str(), a->name.c_str());
}

// 8507 -> FriendAddNoCmd (sub_45D6F0): S2C 8515 [str our name] to the asker, 8547 to us; the asker gone -> 8545.
void FriendServer::no(Client& me, uint32_t asker)
{
    Client* a = by_uid(asker);
    me.busy = false;
    me.pending.clear();
    if (!a) { send(me, 8545); return; }
    a->busy = false;
    if (!a->logged) return;
    PacketBuffer r(static_cast<Opcode>(8515), a->paula);
    write_str(r, me.name);
    send_raw(*a, r);
    send(me, 8547);
}

// 8519 -> FriendAddCancelCmd (sub_45C630): S2C 8520 [str our name] to the asked one (its +152 cleared); gone -> 8545.
void FriendServer::cancel(Client& me, const std::string& name)
{
    me.busy = false;
    Client* t = by_name(name);
    if (!t) { send(me, 8545); return; }
    t->busy = false;
    if (!t->logged) return;
    PacketBuffer c(static_cast<Opcode>(8520), t->paula);
    write_str(c, me.name);
    send_raw(*t, c);
    t->pending.clear();
}

// usp_update_friend_info_set_delete: our row goes; theirs goes too when they are online (1), else it is marked
// deleted (shown at their next login). 0 = done, 101 / 102 the rows.
static int set_delete(Database& db, uint32_t owner, uint32_t friend_uid, int state)
{
    DBTransaction tx(db);
    if (db.execute("DELETE FROM friend_info WHERE ownerid = %u AND friendid = %u", owner, friend_uid) != 1) return 101;
    const uint64_t n = state == 0
        ? db.execute("UPDATE friend_info SET deleted = 1 WHERE ownerid = %u AND friendid = %u", friend_uid, owner)
        : db.execute("DELETE FROM friend_info WHERE ownerid = %u AND friendid = %u", friend_uid, owner);
    if (n != 1) return 102;
    tx.commit();
    return 0;
}

// 8504 -> FriendDelCmd (sub_45FD50 / sub_4600C0): an online friend of our list: S2C 8534 (busy), then
// usp_update_friend_info_set_delete(.., 1) -> 8531 [u64 us][str] to it, 8532 [u64 it][str] to us; not online -> 8580
// (or 8508 [15]); a DB error -> 8580.
void FriendServer::drop(Client& me, const std::string& name)
{
    Client* f = by_name(name);
    if (!f || !has(me, f->uid)) { send(me, 8580); return; }
    send(me, 8534);
    if (!f->logged) {
        PacketBuffer b(static_cast<Opcode>(8508), me.paula);
        b.write<uint8_t>(15);
        send_raw(me, b);
        return;
    }
    const uint32_t fu = f->uid;
    auto drop_entry = [](Client& c, uint32_t uid) {
        for (auto it = c.friends.begin(); it != c.friends.end(); ++it) if (it->uid == uid) { c.friends.erase(it); return; }
    };
    if (!has(*f, me.uid)) {                                   // only our list holds it (retail: removed, nothing sent)
        drop_entry(me, fu);
        return;
    }
    if (set_delete(srv_.db(), me.uid, fu, 1) != 0) { send(me, 8580); return; }
    PacketBuffer a(static_cast<Opcode>(8531), f->paula);
    a.write<uint64_t>(me.uid);
    write_str(a, me.name);
    send_raw(*f, a);
    drop_entry(*f, me.uid);
    drop_entry(me, fu);
    PacketBuffer b(static_cast<Opcode>(8532), me.paula);
    b.write<uint64_t>(fu);
    write_str(b, f->name);
    send_raw(me, b);
    LOG_INFO("FriendServer: %s dropped %s", me.name.c_str(), f->name.c_str());
}

// 8505 -> FriendDelOfflineCmd (sub_460B90 / sub_460C20): S2C 8534, the character by name (usp_select_char_attr_by_name),
// usp_update_friend_info_set_delete(.., 0) -> 8533 [u64 it][str name] (101 / 102: [u64 0]); else 8567.
void FriendServer::drop_offline(Client& me, const std::string& name)
{
    send(me, 8534);
    Database& db = srv_.db();
    auto r = db.query("SELECT uid FROM characters WHERE name = '%s' AND deleted = 0",
                      db.escape(from_client_text(name)).c_str());
    if (r.size() != 1) { send(me, 8567); return; }
    const uint32_t fu = static_cast<uint32_t>(std::stoul(r[0].at("uid")));
    if (Client* f = by_uid(fu)) {
        if (f->logged && has(*f, me.uid)) {                   // "Error CmdList Order Changed": it is online
            PacketBuffer b(static_cast<Opcode>(8508), me.paula);
            b.write<uint8_t>(15);
            send_raw(me, b);
            return;
        }
    }
    const int rc = set_delete(db, me.uid, fu, 0);
    if (rc != 0 && rc != 101 && rc != 102) { send(me, 8567); return; }
    for (auto it = me.friends.begin(); it != me.friends.end(); ++it) if (it->uid == fu) { me.friends.erase(it); break; }
    PacketBuffer o(static_cast<Opcode>(8533), me.paula);
    o.write<uint64_t>(rc == 0 ? fu : 0);
    write_str(o, name);
    send_raw(me, o);
}

// 8538 -> FriendUpdateCmd (sub_471D40): a friend of our list -> S2C 8539 [u64][u8 old][u32 new] +
// usp_update_friend_info_groupnum; else 8540 [u64][u8][u32 10].
void FriendServer::move(Client& me, uint32_t uid, uint8_t old_group, uint32_t group)
{
    Entry* e = nullptr;
    for (Entry& x : me.friends) if (x.uid == uid) { e = &x; break; }
    PacketBuffer p(static_cast<Opcode>(e && group < 5 ? 8539 : 8540), me.paula);
    p.write<uint64_t>(uid);
    p.write<uint8_t>(old_group);
    p.write<uint32_t>(e && group < 5 ? group : 10);
    send_raw(me, p);
    if (!e || group >= 5) return;
    e->group = static_cast<uint8_t>(group);
    srv_.db().execute("UPDATE friend_info SET groupNum = %u WHERE ownerid = %u AND friendid = %u", group, me.uid, uid);
}

// 8541 -> FriendUpdateGroupCmd (sub_4722F0): the group's name (the client shows it itself) +
// usp_update_friend_group_name.
void FriendServer::rename(Client& me, uint32_t group, const std::string& name)
{
    if (group >= 5) return;
    me.groups[group] = from_client_text(name);
    cut_utf8_text(me.groups[group], 20);               // (20 bytes, never inside a character)
    srv_.db().execute("UPDATE friend_group_info SET groupName = '%s' WHERE ownerid = %u AND groupNum = %u",
                      srv_.db().escape(me.groups[group]).c_str(), me.uid, group);
}

// 8556 -> OnCmdSendSMS (sub_46D780): to a friend online whose list holds us -> S2C 8559 [u64 us][str text] to it,
// 8560 [u64 it] to us; else 8557 (not online) / 8558 (not logged in) / 8565 (not our friend) / 8566 (not its).
void FriendServer::note(Client& me, uint32_t to, const std::string& text)
{
    Client* t = by_uid(to);
    if (!t) { send(me, 8557); return; }
    if (!t->logged) { send(me, 8558); return; }
    if (!has(me, to)) { send(me, 8565); return; }
    if (!has(*t, me.uid)) { send(me, 8566); return; }
    std::string line = text;
    cut_client_text(line, kNoteMax);                   // Hardening (not retail): see kNoteMax
    PacketBuffer n(static_cast<Opcode>(8559), t->paula);
    n.write<uint64_t>(me.uid);
    write_str(n, line);
    send_raw(*t, n);
    PacketBuffer a(static_cast<Opcode>(8560), me.paula);
    a.write<uint64_t>(to);
    send_raw(me, a);
}

// 8561 -> FriendSMSOfflineCmd (sub_4717D0 / sub_4718B0): its notes counted (Query_CountSMSByUID) — refused at
// FriendOfflineSMSLimitCount (SystemServer.ini: 5; sub_461820) — then usp_insert_friend_sms(to, us, text (50), 0) ->
// S2C 8563 done / 8564 failed.
void FriendServer::note_offline(Client& me, uint32_t to, const std::string& text)
{
    if (!me.logged) return;
    Database& db = srv_.db();
    auto c = db.query("SELECT COUNT(*) AS n FROM friend_sms WHERE ownerid = %u", to);
    const int n = c.empty() ? 0 : std::stoi(c[0].at("n"));
    bool ok = false;
    if (n < kSmsLimit) {
        std::string raw = text;
        cut_client_text(raw, kNoteMax);                // Hardening (not retail): see kNoteMax
        std::string t = from_client_text(raw);
        cut_utf8_text(t, 50);                          // (the procedure's 50, never inside a character)
        ok = db.execute("INSERT INTO friend_sms (ownerid, sender, dtime, content, sms_kind) VALUES (%u, %u, NOW(), '%s', 0)",
                        to, me.uid, db.escape(t).c_str()) == 1;
    }
    send(me, ok ? 8563 : 8564);
}

// 8573 (sub_46DB20): our client takes no notes (its option) -> S2C 8574 to the sender, when it is our friend.
void FriendServer::note_refused(Client& me, uint32_t from)
{
    if (!has(me, from)) return;
    if (Client* s = by_uid(from)) send(*s, 8574);
}

// 8575 (sub_46DB90): the asked one could not answer (its client was busy) -> S2C 8576 [str its name] to the asker.
void FriendServer::cannot(Client& me, uint32_t asker)
{
    me.busy = false;
    me.pending.clear();
    Client* a = by_uid(asker);
    if (!a) return;
    a->busy = false;
    PacketBuffer p(static_cast<Opcode>(8576), a->paula);
    write_str(p, me.name);
    send_raw(*a, p);
}

// 8551 FriendOfficePosInfoCmd (sub_46AFE0): our login done (else the log, sub_46BD50(11)); the friend's FriendClient (by
// uid) not logged in -> S2C 8545 (sub_46AF50); its office (FriendClient +1316 — here its game session) between maps ->
// 8553 [u16 from][u16 to] (the destination twice: our old session already holds it); else 8552 [u16 zone]; then, not
// between maps and in our map, its game server's FriendObservePosInfoProcessor (agency 182, sub_46B210 -> GameServer
// sub_828BF0 / sub_828D50): S2C 8554 [u16 x / 16][u16 y / 16][u16 zone][u64 its uid] to us on the GAME connection,
// once. (VERIFIED: the client's s2c_friend_8552 / 8553 / 8554.)
void FriendServer::position(Client& me, uint32_t uid)
{
    if (!me.logged) { LOG_INFO("FriendOfficePosInfoCmd::Execute() : requester login not completed"); return; }
    Client* f = by_uid(uid);
    std::shared_ptr<GameClientSession> fs = (f && f->logged) ? srv_.session_of_char(uid) : nullptr;
    int16_t fz = 0;
    bool fm = false;
    int32_t fx = 0, fy = 0;
    if (!fs || !fs->office(fz, fm, fx, fy)) {
        send(me, 8545);
        LOG_INFO("FriendOfficePosInfoCmd::IsNull:owner!=NULL && target==NULL (%u)", uid);
        return;
    }
    if (fm) {
        PacketBuffer p(static_cast<Opcode>(8553), me.paula);
        p.write<uint16_t>(static_cast<uint16_t>(fz));
        p.write<uint16_t>(static_cast<uint16_t>(fz));
        send_raw(me, p);
        return;
    }
    {
        PacketBuffer p(static_cast<Opcode>(8552), me.paula);
        p.write<uint16_t>(static_cast<uint16_t>(fz));
        send_raw(me, p);
    }
    std::shared_ptr<GameClientSession> ms = srv_.session_of_char(me.uid);
    int16_t mz = 0;
    bool mm = false;
    int32_t mx = 0, my = 0;
    if (!ms || !ms->office(mz, mm, mx, my) || mz != fz) return;
    PacketBuffer p(static_cast<Opcode>(8554), ms->is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(fx / 16));
    p.write<uint16_t>(static_cast<uint16_t>(fy / 16));
    p.write<uint16_t>(static_cast<uint16_t>(fz));
    p.write<uint64_t>(uid);
    srv_.send_to_session(ms->session_id(), p);
}

// 8586 FriendChangeZoneToCmd (sub_45F200): our login done (else the log); the friend not logged in -> S2C 8513; between
// maps -> 8588 [u8 0] ("ChangeZoneTo_MapMoving"); its map ours -> 8588 [1] ("Samezone"); else its game server's
// FriendObservePosInfoReturnProcessor (agency 182, sub_828F60: [u16 x][u16 y][u16 zone], none: zeros) -> OnAgencyReport
// (sub_45F510): a zone 1..999 -> S2C 8587 [x][y][zone] (the client puts the six bytes into its Friend Finder's custom data
// and uses it: Effect 39, item_use.cpp), else 8588 [2] ("NotReachable"). (VERIFIED: the client's s2c_friend_8587 / 8588;
// its sender sub_857CB0 needs an Effect-39 item in the bag.) A friend logged in without a game session here is taken as
// between maps.
void FriendServer::change_zone_to(Client& me, uint32_t uid)
{
    if (!me.logged) { LOG_INFO("FriendChangeZoneToCmd::Execute() : requester login not completed"); return; }
    Client* f = by_uid(uid);
    if (!f || !f->logged) { send(me, 8513); return; }
    auto reply = [&](uint8_t code) {
        PacketBuffer p(static_cast<Opcode>(8588), me.paula);
        p.write<uint8_t>(code);
        send_raw(me, p);
    };
    std::shared_ptr<GameClientSession> fs = srv_.session_of_char(uid);
    int16_t fz = 0;
    bool fm = false;
    int32_t fx = 0, fy = 0;
    if (!fs || !fs->office(fz, fm, fx, fy) || fm) { reply(0); return; }
    std::shared_ptr<GameClientSession> ms = srv_.session_of_char(me.uid);
    int16_t mz = 0;
    bool mm = false;
    int32_t mx = 0, my = 0;
    if (ms && ms->office(mz, mm, mx, my) && mz == fz) { reply(1); return; }
    if (fz < 1 || fz >= 1000) { reply(2); return; }
    PacketBuffer p(static_cast<Opcode>(8587), me.paula);
    p.write<uint16_t>(static_cast<uint16_t>(fx));
    p.write<uint16_t>(static_cast<uint16_t>(fy));
    p.write<uint16_t>(static_cast<uint16_t>(fz));
    send_raw(me, p);
    LOG_INFO("FriendServer: %u goes to %u: zone %d (%d,%d)", me.uid, uid, fz, fx, fy);
}
