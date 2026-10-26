#include "party.hpp"
#include "game_server.hpp"
#include "game_session.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  The party (see party.hpp). Every S2C below is read by the client's dispatch_char_7000 (sub_89F450):
//    7002 [u16 party][str name][u16 master]      the party formed (the asker; s2c_char_7002_info)
//    7003 [u8 code]                              making it failed (the maker)
//    7005 / 7006 [u8]                            the division changed / changing it failed (the master)
//    7008 / 7009 [u8]                            the name changed / changing it failed (the master)
//    7012 [u8]                                   changing the master failed (7011, its success, has no
//                                                client case — not sent)
//    7014 [str name]                             the one asked accepted (the asker, sub_9C7C70)
//    7015 [str name][u8 code]                    asking failed / refused (the asker, sub_9C3300)
//    7016 [str party][str asker][u8 members][u8 division][u8 item mode][u16 avg level]   the invitation
//    7018 [u16 party][str name][u16 master][u16 me][u8 division][u8 n][u16 force normal][u8 item mode]
//         [u8 requests welcome][n x record][u8 dull n][dull records]  the party, to whom joined
//    7019 [u8 code]                              accepting failed
//    7021                                        we left (the client clears its party)
//    7026 / 7027 [u8]                            kicked / kicking failed (the master)
//    7033 [record][u16 force normal]             a member joined (the others)
//    7034 [u16 who][u16 master]                  a member left
//    7035 [u16 who][u16 zone] / 7036 [u16 who][u16 zone]   a member moving / in a zone
//    7037 [u16 who]                              a member kicked (the party and the kicked)
//    7038 [str name]                             the party's new name
//    7039 [u8 type][u8 n][n x (u16 who, u8 %)]   the EXP division
//    7040 [u16 who][u8 mask][...]                a member's numbers, to its same-zone mates (the GameServer's
//                                                PartyObserveAndNotifyProcessor sub_86C110: 1 HP u32, 2 MP u32,
//                                                4 x/16 u16, 8 y/16 u16, 0x10 level u16, 0x20 MaxHP u32,
//                                                0x40 MaxMP u32)
//    7044 [u16 master][u8 force normal]          a new master
//    7045 [u16 who][u64 uid][u16 force normal]   a dull member came back under a new id
//    7046 [u16 who][u16 master]                  a member went dull (disconnected)
//    7048 / 7050 [u64 uid]                       a dull member dropped (by the master / the depot's end)
//    7049 [u64 uid]                              (the master) no such dull member
//    7051 [u16 party][u16 me]                    the party made, waiting (the maker)
//    7052                                        the party broke up
//    7055                                        changing the item mode failed (Alert_ItemShareFail)
//    7056 [u8 mode]                              the item mode
//    7066 [str who][u16][u32]                    (the master) someone asks to join (ui_partyapply)
//    7068 / 7071 / 7082 [u32 code]               a join request / its answer / a party look-up failed
//    7072 / 7083                                 (the asker) refused (Alert_Denied) / the invitation is gone
//    7076 [u16 party][str who][str master][u16 avg lv][u8 type][u8 n][u8 division][u8 item mode]
//                                                (the maker) the one asked is in a party: ask to join it?
//    7078 [u8]                                   join requests welcome or not (client +209: when 0 it
//                                                answers a 7066 with C2S 7072 by itself)
//    7081 [u16 party][str who][u16 avg lv][u8 type][u8 n][u8 division][u8 item mode]   the party of a name
//    7086 [u8 party type][f32 bonus - 1]         our share group's bonus (client +200, the party type icon)
//  Record (60 bytes, client sub_89BF10 copies it, sub_B20280 draws it): name[20] +0, u16 type +20,
//  u8 +22, u16 zone +23, u16 member id +25 (the key), u32 HP +27, MP +31, MaxHP +35, MaxMP +39,
//  u16 level +43, u16 x/16 +45, y/16 +47, u8 +49, +50, u64 uid +51, u8 dull +59.
//  The rates of a share group go out as S2C 886 (EXP) / 887 (TM): the 2014 client reads 10 f32 (a sum it
//  shows as "+N%"; an 11th when the 9th is > 0) — the 2012 GameServer wrote its 8 (sub_604D90 / sub_604E30).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr int     kPartyLimitLevel  = 1;          // SystemServer.ini PartyLimitLevel (Paula)
constexpr int32_t kDepotIntervalMs  = 6000000;    // SystemServer.ini PartyMemberDepotInterval (Paula)
constexpr size_t  kNameMax          = 35;         // Party +10: strncpy_s(36, _TRUNCATE)

// sub_50C240: a custom share (division 2) must lie within [min, max] of the table's count.
constexpr int kDivMin[7] = { 0, 0, 40, 20, 10, 10, 10 };        // SystemServer 0x6C5ED0
constexpr int kDivMax[7] = { 10, 100, 60, 50, 40, 36, 20 };     // dword_6C5EE8

// The party bonus by party type (PartyType.cpp ids) and the share group's size (SystemServer
// flt_6C79C0 = EXP, sub_51A410; flt_6C7B08 = TM, sub_51A450).
constexpr float kBonusExp[11][7] = {
    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },     // 0 NonePartyType
    { 1.0f, 1.0f, 1.2f, 1.3f, 1.3f, 1.4f, 1.5f },     // 1 PartyTypeNormal
    { 1.0f, 1.0f, 1.3f, 1.5f, 1.5f, 2.0f, 2.5f },     // 2 PartyTypeRoyal
    { 1.0f, 1.0f, 1.2f, 1.4f, 1.4f, 1.7f, 2.0f },     // 3 PartyTypeType
    { 1.0f, 1.0f, 1.3f, 1.5f, 1.5f, 2.0f, 2.5f },     // 4 PartyTypeHomo
    { 1.0f, 1.0f, 1.2f, 1.3f, 1.3f, 1.4f, 1.5f },     // 5 PartyTypeFull
    { 1.0f, 1.0f, 1.2f, 1.3f, 1.3f, 1.4f, 1.5f },     // 6 PartyTypeSeparate
    { 1.0f, 1.0f, 1.3f, 1.5f, 1.5f, 2.0f, 2.5f },     // 7 PartyTypeLevel
    { 1.0f, 1.0f, 1.3f, 1.5f, 1.5f, 2.0f, 2.5f },     // 8 PartyTypeMultipleLv
    { 1.0f, 1.0f, 1.3f, 1.4f, 1.4f, 1.6f, 1.8f },     // 9 PartyTypeBeginner
    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },     // 10 PartyTypeFixed
};
constexpr float kBonusTm[11][7] = {
    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 1.2f, 1.3f, 1.3f, 1.5f, 1.8f },
    { 1.0f, 1.0f, 1.3f, 1.5f, 1.5f, 2.5f, 3.0f },
    { 1.0f, 1.0f, 1.2f, 1.4f, 1.4f, 2.0f, 2.5f },
    { 1.0f, 1.0f, 1.3f, 1.5f, 1.5f, 2.5f, 3.0f },
    { 1.0f, 1.0f, 1.2f, 1.3f, 1.3f, 1.5f, 1.8f },
    { 1.0f, 1.0f, 1.2f, 1.3f, 1.3f, 1.5f, 1.8f },
    { 1.0f, 1.0f, 1.3f, 1.5f, 1.5f, 2.5f, 3.0f },
    { 1.0f, 1.0f, 1.3f, 1.5f, 1.5f, 2.5f, 3.0f },
    { 1.0f, 1.0f, 1.3f, 1.5f, 1.5f, 1.8f, 2.0f },
    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
};

void put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

void write_str(PacketBuffer& p, const std::string& s) { p.write_bytes(s.c_str(), s.size() + 1); }

bool same_name(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// sub_50FBF0 / sub_50FB00: a character type's line (0 Bunny/Buffalo .. 3 Cat/Raccoon) and sex, for the
// three tiers of models 1..24 (another type: 0, 0).
int type_line(int type)   { return (type >= 1 && type <= 24) ? ((type - 1) % 8) / 2 : 0; }
int type_gender(int type) { return (type >= 1 && type <= 24) ? ((type - 1) % 2) : 0; }

// sub_518210: the first PartyType (PartyType.cpp, in its table's order) that takes the list {line, sex,
// level} — the master's first, the members, the dull ones. Normal takes any; Full and Separate, after
// it, never come.
struct TypeInfo { int line; int gender; int level; };
int match_party_type(std::vector<TypeInfo> v)
{
    const size_t n = v.size();
    if (n == 0) return 0;                                                   // NonePartyType
    {
        int lo = 0xFFFF, hi = 0;
        for (const TypeInfo& e : v) { lo = std::min(lo, e.level); hi = std::max(hi, e.level); }
        if (hi - lo >= 100) return 10;                                      // PartyTypeFixed
    }
    for (const TypeInfo& e : v) if (e.level <= 30) return 9;                // PartyTypeBeginner
    if (n >= 4) {
        bool all = true;
        for (size_t i = 1; i < n; ++i) if (v[i].level != v[0].level) all = false;
        if (all) return 7;                                                  // PartyTypeLevel
        for (size_t i = 0; i + 1 < n; ++i)                                  // PartyTypeMultipleLv (its bubble
            for (size_t j = 0; j + 1 < n; ++j)                              // sort stays in the list)
                if (v[j].level > v[j + 1].level) std::swap(v[j], v[j + 1]);
        bool mult = true;
        for (size_t k = 1; k < n; ++k) if (v[k].level != v[0].level * static_cast<int>(k + 1)) mult = false;
        if (mult) return 8;
        bool homo = true;
        for (size_t i = 1; i < n; ++i) if (v[i].gender != v[0].gender) homo = false;
        if (homo) return 4;                                                 // PartyTypeHomo
        bool seen[4] = {};
        for (const TypeInfo& e : v) seen[e.line & 3] = true;
        if (seen[0] && seen[1] && seen[2] && seen[3]) return 2;             // PartyTypeRoyal
        bool line = true;
        for (size_t i = 1; i < n; ++i) if (v[i].line != v[0].line) line = false;
        if (line) return 3;                                                 // PartyTypeType
    }
    return 1;                                                               // PartyTypeNormal
}

} // namespace

// ── lookups ──────────────────────────────────────────────────────────────────

PartyServer::Client* PartyServer::client(uint32_t uid)
{
    auto it = by_uid_.find(uid);
    return it == by_uid_.end() ? nullptr : client_id(it->second);
}

PartyServer::Client* PartyServer::client_id(uint16_t id)
{
    auto it = clients_.find(id);
    return it == clients_.end() ? nullptr : &it->second;
}

// sub_50CC50: an online character by name.
PartyServer::Client* PartyServer::client_name(const std::string& name)
{
    for (auto& kv : clients_)
        if (kv.second.sid && !kv.second.moving && same_name(kv.second.info.name, name)) return &kv.second;
    return nullptr;
}

PartyServer::Party* PartyServer::party_of(const Client& c)
{
    return c.party ? party_id(c.party) : nullptr;
}

PartyServer::Party* PartyServer::party_id(uint16_t id)
{
    auto it = parties_.find(id);
    return it == parties_.end() ? nullptr : &it->second;
}

// sub_50D150: the pending invitation still stands (its party is there and its asker in it), else gone.
bool PartyServer::asked(Client& c)
{
    if (c.asked) {
        auto it = parties_.find(c.asked_party);
        if (it != parties_.end() &&
            std::find(it->second.members.begin(), it->second.members.end(), c.asker) != it->second.members.end())
            return true;
    }
    clear_asked(c);
    return false;
}

void PartyServer::clear_asked(Client& c)
{
    c.asked = false;
    c.asked_party = 0;
    c.asker = 0;
}

// sub_4F4850: the average level of the members and the dull ones.
int PartyServer::avg_level(const Party& p)
{
    int sum = 0;
    for (uint16_t id : p.members)
        if (Client* m = client_id(id)) sum += m->info.level;
    for (const Dull& d : p.dull) sum += d.level;
    const size_t n = p.members.size() + p.dull.size();
    return n ? static_cast<int>(static_cast<double>(sum) / static_cast<double>(n)) : 0;
}

// sub_4EFDF0: a level the party takes (PartyLimitLevel) while its master is with it; clears the
// force-normal mode.
bool PartyServer::level_ok(Party& p, int level)
{
    if (level < kPartyLimitLevel) return false;
    if (!has_master(p)) return false;
    p.force_normal = false;
    return true;
}

// sub_4EFC00: the master is in the member list.
bool PartyServer::has_master(const Party& p) const
{
    return std::find(p.members.begin(), p.members.end(), p.master) != p.members.end();
}

// The SystemServer's packets go out on the character's community connection (it stays up through a
// field change); without one, on the game connection — the client routes both alike (post_game_event ->
// dispatch_packet_main -> dispatch_char_7000).
void PartyServer::send(const Client& c, const PacketBuffer& p)
{
    auto it = systems_.find(c.uid);
    if (it != systems_.end()) { gs_.send_to_system(it->second, p); return; }
    if (c.sid && !c.moving) gs_.send_to_session(c.sid, p);
}

// The GameServer's own (S2C 886 / 887 rates, 7040 observers): the game connection.
void PartyServer::send_game(const Client& c, const PacketBuffer& p)
{
    if (c.sid && !c.moving) gs_.send_to_session(c.sid, p);
}

void PartyServer::attach_system(uint32_t uid, uint32_t sys_sid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    systems_[uid] = sys_sid;
}

void PartyServer::detach_system(uint32_t uid, uint32_t sys_sid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    auto it = systems_.find(uid);
    if (it != systems_.end() && it->second == sys_sid) systems_.erase(it);
}

void PartyServer::send_uid(uint32_t uid, const PacketBuffer& p)
{
    if (Client* c = client(uid)) send(*c, p);
}

// sub_4F1B30: to every member.
void PartyServer::send_party(const Party& pt, const PacketBuffer& p, uint16_t except)
{
    for (uint16_t id : pt.members)
        if (id != except)
            if (Client* m = client_id(id)) send(*m, p);
}

// sub_4F4220: to the master alone.
void PartyServer::send_master(const Party& pt, const PacketBuffer& p)
{
    if (!has_master(pt)) {
        LOG_ERROR("PARTY  Party::SendToPartyMaster:FailTofindMaster at Party:%u:", pt.id);
        return;
    }
    if (Client* m = client_id(pt.master)) send(*m, p);
}

void PartyServer::write_record(PacketBuffer& p, const Client& c) const
{
    uint8_t r[60]{};
    std::memcpy(r, c.info.name.c_str(), std::min<size_t>(c.info.name.size(), 19));
    put16(r + 20, c.info.type);
    put16(r + 23, static_cast<uint16_t>(c.moving ? 0 : c.info.zone));
    put16(r + 25, c.id);
    put32(r + 27, static_cast<uint32_t>(c.info.hp));
    put32(r + 31, static_cast<uint32_t>(c.info.mp));
    put32(r + 35, static_cast<uint32_t>(c.info.max_hp));
    put32(r + 39, static_cast<uint32_t>(c.info.max_mp));
    put16(r + 43, static_cast<uint16_t>(c.info.level));
    put16(r + 45, static_cast<uint16_t>(c.info.x / 16));
    put16(r + 47, static_cast<uint16_t>(c.info.y / 16));
    put64(r + 51, c.uid);
    p.write_bytes(r, sizeof(r));
}

// ── the session's life ───────────────────────────────────────────────────────

void PartyServer::enter(uint32_t uid, uint32_t sid, const PartyInfo& info, bool paula)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    paula_ = paula;
    if (Client* c = client(uid)) {                     // back after a field change
        const bool was_moving = c->moving;
        c->sid = sid;
        c->moving = false;
        c->dead = false;                               // (+36 = 0)
        c->info = info;
        c->sent = PartyInfo{};
        c->type_bonus = 0;                             // (the new player object has none yet)
        c->share = PartyShare{};
        if (Party* pt = party_of(*c)) {                // Party::OnNotifyEnterZone (sub_4F1DE0)
            party_type(*pt);
            PacketBuffer p = pkt(7036);
            p.write<uint16_t>(c->id);
            p.write<uint16_t>(static_cast<uint16_t>(info.zone));
            send_party(*pt, p);
            // A new login while the old connection still lingered: its client knows no party yet
            // (retail would make it dull, then rejoin it with S2C 7045 + 7018) — the party, as to a joiner.
            if (!was_moving) send_party_info(*pt, *c);
            observe(*pt);                              // sub_4F0610 -> sub_4EFE40
            order_share(*pt);                          // sub_4F0780 (ApplyBonus)
            pq_entered(*pt, *c);                       // the party quest (+ sub_4F04E0 when it plays)
        }
        // A board balloon is the new player object's again (ours: the head notice is not carried by the
        // zone change otherwise).
        for (const BoardParty& r : board_parties_) if (r.balloon == uid) balloon(uid, 1, r.comment);
        for (const BoardSeeker& r : board_seekers_) if (r.uid == uid) balloon(uid, 2, r.comment);
        return;
    }
    uint16_t id = next_member_;
    while (id == 0 || clients_.count(id)) ++id;
    next_member_ = static_cast<uint16_t>(id + 1);
    Client c;
    c.id = id;
    c.uid = uid;
    c.sid = sid;
    c.info = info;
    clients_[id] = c;
    by_uid_[uid] = id;

    // RegisterDullCharacter (sub_514BE0) + ActivateDullPartyClient (sub_514AD0) on its first zone: a
    // character kept in the PartyMemberDepot rejoins its party (sub_50F6F0: +37 = 1, add_member).
    auto it = depot_.find(uid);
    if (it == depot_.end()) return;
    const uint16_t pid = it->second.party;
    depot_.erase(it);
    Party* pt = party_id(pid);
    if (!pt) {
        LOG_INFO("PartyServer::RegisterDullCharacter: That Party(%u) Was Broken.", pid);
        return;
    }
    auto d = std::find_if(pt->dull.begin(), pt->dull.end(), [&](const Dull& x) { return x.uid == uid; });
    if (d == pt->dull.end()) {
        LOG_ERROR("PARTY      Party::ActivateDullMember:WrongDullMemberCUID:%u:", uid);
        return;
    }
    Client& me = clients_[id];
    me.party = pt->id;
    add_member(*pt, me, true);
    pq_entered(*pt, me);                               // its party quest back (PartyOrderRecoverQuest)
}

void PartyServer::leave_zone(uint32_t uid, int16_t dest_zone)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    if (Party* pt = party_of(*c)) {                    // sub_4F1F40 (no new share: that waits for the zone)
        c->moving = true;                              // (+32 = 0)
        party_type(*pt);
        PacketBuffer p = pkt(7035);
        p.write<uint16_t>(c->id);
        p.write<uint16_t>(static_cast<uint16_t>(dest_zone));
        send_party(*pt, p);
    }
    c->moving = true;
}

// The session closed. One on the move (a field change: the client drops this connection for a new one)
// keeps its place for the next session (enter); if none comes within a minute, tick() lets it go.
void PartyServer::closed(uint32_t uid, uint32_t sid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c || c->sid != sid) return;                   // another session has it now
    if (c->moving) {
        c->sid = 0;
        c->moving_ms = 0;
        return;
    }
    drop(*c);
}

// UnregisterCharacter (sub_513E10 -> sub_513A00 -> sub_50D290): an invitation to it is refused (code 22)
// and the member leaves its party "temporarily" (sub_50F5E0).
void PartyServer::drop(Client& cref)
{
    Client* c = &cref;
    const uint32_t uid = c->uid;
    if (asked(*c)) {
        if (Client* a = client_id(c->asker)) {
            PacketBuffer p = pkt(7015);                // sub_50E850(22)
            write_str(p, c->info.name);
            p.write<uint8_t>(22);
            send(*a, p);
        }
        clear_asked(*c);
    }
    if (c->party) leave_temporarily(*c);
    board_remove_seeker(uid);                          // sub_513E10: sub_4FCE90
    LOG_INFO("PARTY SERVER   DELETE PARTYMEMBER - CHRUID(%u), PTYMEMBERID(%u)", uid, c->id);
    by_uid_.erase(uid);
    clients_.erase(c->id);
}

// The PartyObserveAndNotifyProcessor of the member (sub_86C400 every 500 ms -> sub_86C110): what changed
// goes to its same-zone mates as S2C 7040.
void PartyServer::update(uint32_t uid, const PartyInfo& info)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c || c->moving) return;
    c->info = info;
    Party* pt = party_of(*c);
    if (!pt) return;
    PartyInfo now = info;
    now.x = static_cast<int16_t>(info.x / 16);
    now.y = static_cast<int16_t>(info.y / 16);
    uint8_t mask = 0;
    if (now.hp     != c->sent.hp)     mask |= 0x01;
    if (now.mp     != c->sent.mp)     mask |= 0x02;
    if (now.x      != c->sent.x)      mask |= 0x04;
    if (now.y      != c->sent.y)      mask |= 0x08;
    if (now.level  != c->sent.level)  mask |= 0x10;
    if (now.max_hp != c->sent.max_hp) mask |= 0x20;
    if (now.max_mp != c->sent.max_mp) mask |= 0x40;
    if (!mask) return;
    c->sent = now;
    PacketBuffer p = pkt(7040);
    p.write<uint16_t>(c->id);
    p.write<uint8_t>(mask);
    if (mask & 0x01) p.write<uint32_t>(static_cast<uint32_t>(now.hp));
    if (mask & 0x02) p.write<uint32_t>(static_cast<uint32_t>(now.mp));
    if (mask & 0x04) p.write<uint16_t>(static_cast<uint16_t>(now.x));
    if (mask & 0x08) p.write<uint16_t>(static_cast<uint16_t>(now.y));
    if (mask & 0x10) p.write<uint16_t>(static_cast<uint16_t>(now.level));
    if (mask & 0x20) p.write<uint32_t>(static_cast<uint32_t>(now.max_hp));
    if (mask & 0x40) p.write<uint32_t>(static_cast<uint32_t>(now.max_mp));
    for (uint16_t id : pt->members) {
        if (id == c->id) continue;
        Client* m = client_id(id);
        if (m && !m->moving && m->info.zone == info.zone) send_game(*m, p);
    }
}

// sub_4EFE40 (OrderRefreshObserve): every member's processor starts over — its next look sends its mates
// all it has.
void PartyServer::observe(Party& pt)
{
    for (uint16_t id : pt.members)
        if (Client* m = client_id(id)) m->sent = PartyInfo{};
}

// Party::OnNotifyDead (sub_4F4C70, "MEMBER DEAD"): the dead member is out of the share. (Player::OnDie
// also runs its StateNotifierToPartyMembers, sub_760D10 -> sub_86BCC0: all its numbers to its mates — the
// next update() sends them, HP 0.)
void PartyServer::dead(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    c->dead = true;
    if (Party* pt = party_of(*c)) {
        LOG_INFO("PARTY SERVER   PARTY - %u, MEMBER DEAD - CHRUID : %u", pt->id, uid);
        c->sent = PartyInfo{};
        order_share(*pt);
    }
}

// sub_4F4CB0 ("MEMBER REBIRTH"): the observers start over and the share takes it back.
void PartyServer::revived(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    c->dead = false;
    if (Party* pt = party_of(*c)) {
        LOG_INFO("PARTY SERVER   PARTY - %u, MEMBER REBIRTH - CHRUID : %u", pt->id, uid);
        observe(*pt);
        order_share(*pt);
        if (pt->pq.qid) pq_notify(*pt);                // sub_4F4CB0: the party quest's update again
    }
}

// PartyMemberDepot (sub_510470 / sub_5102D0): 10 buckets of PartyMemberDepotInterval / 10 each; a dull
// character pushed in one is dropped when its bucket comes round again (after 9/10 .. 10/10 of the interval).
void PartyServer::tick(int32_t elapsed_ms)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    {                                                  // a field change that never came back
        std::vector<uint16_t> lost;
        for (auto& kv : clients_)
            if (kv.second.moving && !kv.second.sid && (kv.second.moving_ms += elapsed_ms) >= 60000)
                lost.push_back(kv.first);
        for (uint16_t id : lost)
            if (Client* c = client_id(id)) {
                c->moving = false;
                drop(*c);
            }
    }
    constexpr int32_t bucket = kDepotIntervalMs / 10;
    depot_ms_ += elapsed_ms;
    while (depot_ms_ >= bucket) {
        depot_ms_ -= bucket;
        ++depot_period_;
        std::vector<std::pair<uint32_t, uint16_t>> gone;
        for (const auto& kv : depot_)
            if (depot_period_ - kv.second.period >= 10) gone.emplace_back(kv.first, kv.second.party);
        for (const auto& g : gone) {
            depot_.erase(g.first);
            delete_dull(g.first, g.second);
        }
    }
}

// sub_514940 DeleteDullPartyClient: the depot's end of a dull member — out of the dull list (S2C 7050),
// then the division, or the party breaks up below 2.
void PartyServer::delete_dull(uint32_t uid, uint16_t party)
{
    Party* pt = party_id(party);
    if (!pt) {
        LOG_ERROR("PARTY SERVER   PartyServer::DeleteDullPartyClient:NotParty:%u:", party);
        return;
    }
    auto d = std::find_if(pt->dull.begin(), pt->dull.end(), [&](const Dull& x) { return x.uid == uid; });
    if (d != pt->dull.end()) {                         // sub_4F42F0
        LOG_INFO("PARTY(%u) Party::DepotDelMemberFromDullList: CHRUID(%u)", pt->id, uid);
        pt->dull.erase(d);
    }
    PacketBuffer p = pkt(7050);
    p.write<uint64_t>(uid);
    send_party(*pt, p);
    if (pt->members.size() + pt->dull.size() >= 2) division(*pt);
    else disband(*pt);
}

// ── the share (the GameServer half) ──────────────────────────────────────────

PartyShare PartyServer::share_of(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    return c ? c->share : PartyShare{};
}

std::vector<uint32_t> PartyServer::group_sessions(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    std::vector<uint32_t> out;
    Client* c = client(uid);
    if (!c || !c->share.in_group) return out;
    for (const PartyShare::Slot& s : c->share.members) {
        if (s.uid == uid) continue;
        if (Client* m = client(s.uid)) if (m->sid) out.push_back(m->sid);
    }
    return out;
}

// S2C 886 / 887 (sub_8880B0 -> sub_604D90 / sub_604E30): the EXP and TM rate slots — ours is slot 0.
void PartyServer::send_rates(const Client& c)
{
    PacketBuffer e = pkt(886);
    e.write<float>(c.share.exp_rate);
    for (int i = 1; i < 10; ++i) e.write<float>(0.0f);
    send_game(c, e);
    PacketBuffer t = pkt(887);
    t.write<float>(c.share.tm_rate);
    for (int i = 1; i < 10; ++i) t.write<float>(0.0f);
    send_game(c, t);
}

// A processor released (sub_86C860 -> sub_86CF10(1)): the rate slot 0 back to 0, the shares to 1.0.
void PartyServer::clear_share(Client& c)
{
    if (!c.share.in_group) return;
    c.share = PartyShare{};
    send_rates(c);
}

// sub_4F0E50: the party type of the list {master, members, dull members} (sub_518210), kept at +632.
int PartyServer::party_type(const Party& pt)
{
    if (!has_master(pt)) return pt.ptype;
    std::vector<TypeInfo> v;
    auto info_of = [&](Client* m) { return TypeInfo{ type_line(m->info.type), type_gender(m->info.type), m->info.level }; };
    if (Client* m = client_id(pt.master)) v.push_back(info_of(m));
    for (uint16_t id : pt.members)
        if (id != pt.master)
            if (Client* m = client_id(id)) v.push_back(info_of(m));
    for (const Dull& d : pt.dull) v.push_back(TypeInfo{ type_line(d.type), type_gender(d.type), d.level });
    const int t = match_party_type(v);
    const_cast<Party&>(pt).ptype = t;
    return t;
}

// sub_4F0780 (ApplyBonus) -> PartyExpShareVisitor::OrderShareExp (sub_518D60) + PartyBonusVisitor: the
// old processors go, then the living, online members are grouped by zone (a member on the move is
// dropped), each group gets a processor (the GameServer's PartyShareExpProcessor, sub_86CAC0): the
// party bonus of its size and party type (the rate slot 0 of each member, + 0.005), the percentages of
// the division (0: 100 each, no sharing; 1: 100/n rounded; 2/3: the table's, scaled to the group) —
// S2C 886 / 887 to each, and 7086 [type][bonus - 1].
void PartyServer::order_share(Party& pt)
{
    for (uint16_t id : pt.members)
        if (Client* m = client_id(id)) clear_share(*m);
    if (!has_master(pt) || pt.members.empty()) return;
    party_type(pt);
    if (pt.ptype == 10) pt.division = 0;               // PartyTypeFixed: each its own (sub_50C140(0), no S2C)

    std::vector<Client*> list;
    for (uint16_t id : pt.members) {
        Client* m = client_id(id);
        if (!m || m->party != pt.id || m->dead || !m->sid) continue;
        if (list.size() < 6) list.push_back(m);
    }
    size_t processed = 0;
    for (int j = 0; j < 6 && processed != list.size(); ++j) {
        std::vector<Client*> group;
        Client* leader = nullptr;
        for (Client*& m : list) {
            if (!m) continue;
            const int16_t z = m->moving ? 0 : m->info.zone;
            if (!leader) {
                leader = m;
                if (z == 0) { m = nullptr; ++processed; leader = nullptr; continue; }
            }
            const int16_t lz = leader->moving ? 0 : leader->info.zone;
            if (z == lz) { group.push_back(m); m = nullptr; }
            else if (z == 0) { m = nullptr; ++processed; }
        }
        processed += group.size();
        if (!leader || group.empty()) continue;

        const size_t n = group.size();
        float a = 1.0f, b = 1.0f;
        bool on = false;
        std::vector<int> pcts(n, 100);
        if (n > 1) {
            a = kBonusExp[pt.ptype][n];
            b = kBonusTm[pt.ptype][n];
            switch (pt.division) {
            case 0:
                break;
            case 1:
                on = true;
                for (int& p : pcts) p = static_cast<int>(std::floor(100.0 / static_cast<double>(n) + 0.5));
                break;
            case 2:
            case 3: {
                on = true;
                int sum = 0;
                std::vector<int> tp(n, 0);
                for (size_t i = 0; i < n; ++i) {
                    for (const DivEntry& e : pt.table) if (e.id == group[i]->id) tp[i] = e.pct;   // sub_50C3C0
                    sum += tp[i];
                }
                for (size_t i = 0; i < n; ++i)
                    pcts[i] = sum ? static_cast<int>(std::floor(static_cast<double>(tp[i]) / static_cast<double>(sum) *
                                                                100.0 + 0.5)) : 0;
                break;
            }
            default:
                LOG_ERROR("PartyExpShareVisitor::OrderShareExp: Invalid... diveType = %d", pt.division);
                continue;
            }
        }
        std::vector<PartyShare::Slot> slots;
        for (size_t i = 0; i < n; ++i) {
            const float pf = static_cast<float>(static_cast<double>(static_cast<uint16_t>(pcts[i])) * 0.009999999776482582);
            slots.push_back(PartyShare::Slot{ group[i]->uid, pf, pf });
        }
        const float exp_rate = static_cast<float>(static_cast<double>(static_cast<float>(a - 1.0)) + 0.004999999888241291);
        const float tm_rate  = static_cast<float>(static_cast<double>(static_cast<float>(b - 1.0)) + 0.004999999888241291);
        for (size_t i = 0; i < n; ++i) {
            PacketBuffer p = pkt(7086);
            p.write<uint8_t>(static_cast<uint8_t>(pt.ptype));
            p.write<float>(static_cast<float>(a - 1.0));
            send(*group[i], p);
        }
        for (size_t i = 0; i < n; ++i) {
            PartyShare& s = group[i]->share;
            s.in_group = true;
            s.on       = on;
            s.items    = pt.items != 0;
            s.exp_rate = exp_rate;
            s.tm_rate  = tm_rate;
            s.exp_pct  = std::min(slots[i].exp_pct, 1.0f);   // sub_86DE50
            s.tm_pct   = std::min(slots[i].tm_pct, 1.0f);    // sub_86DE90
            s.members  = slots;
            send_rates(*group[i]);
        }
        LOG_DEBUG("PARTY SERVER   PARTY - %u, ORDER SHARE EXP - TYPE %d, DIVISION %u, %zu MEMBER(S) IN ZONE %d",
                 pt.id, pt.ptype, pt.division, n, static_cast<int>(leader->info.zone));
    }

    // PartyBonusVisitor: the type bonus (the GameServer's PartyTypeBonusProcessor) — a Homo party
    // (sub_51A110: 1, the faster recovery) or a Type party (sub_519EA0: 2..5 by the master's line, +4
    // points to its three stats) for the members in the master's zone; every other type cancels it.
    Client* master = client_id(pt.master);
    const int16_t mz = (master && !master->moving) ? master->info.zone : 0;
    uint8_t bonus = 0;
    if (pt.ptype == 4) bonus = 1;
    else if (pt.ptype == 3 && master) bonus = static_cast<uint8_t>(2 + type_line(master->info.type));
    for (uint16_t id : pt.members) {
        Client* m = client_id(id);
        if (!m) continue;
        const uint8_t b = (bonus && mz && !m->moving && m->info.zone == mz) ? bonus : 0;
        if (b != m->type_bonus) {
            m->type_bonus = b;
            if (m->sid) gs_.post_to_session(m->sid, [b](GameClientSession& s) { s.party_type_bonus(b); });
        }
    }
}

// The PartyRewardHandler of uid's group (sub_86DC40 EXP / sub_86CCD0 TM): sharing (+113) — every member
// whose share of it is worth something ((int)(amount * share + 0.99) != 0) gets event 30037 / 30127 with
// the whole amount (its own rates then take its share); else the earner alone. False: no group, or
// nobody got it (the caller adds it the usual way).
bool PartyServer::reward(uint32_t uid, int32_t amount, uint8_t flag, bool tm)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c || !c->share.in_group) return false;
    const PartyShare s = c->share;
    bool handled = false;
    for (const PartyShare::Slot& slot : s.members) {
        if (!s.on && slot.uid != uid) continue;
        Client* m = client(slot.uid);
        if (!m || !m->sid || m->moving) continue;      // (the processor's slot lost its player)
        if (!s.on) handled = true;
        const float v = static_cast<float>(static_cast<double>(amount) * (tm ? slot.tm_pct : slot.exp_pct));
        if (static_cast<int>(v + 0.99f) != 0) {
            handled = true;
            gs_.post_to_session(m->sid, [amount, flag, tm](GameClientSession& gsess) {
                if (tm) gsess.party_tm(amount, flag);
                else    gsess.party_exp(amount, flag);
            });
        }
        if (!s.on) break;
    }
    return handled;
}

// PartyEnemyJudge / PartyMemberJudge (sub_86DB90 / sub_86DBF0): `other` holds one of uid's group's slots.
bool PartyServer::same_group(uint32_t uid, uint32_t other)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    return c && c->share.in_group && c->share.has(other);
}

// The pick-up of an owned item (Player sub_73D2A0 @0x749740 / @0x75055A): its owner's group holds the
// picker (PartyMemberJudge) and shares items (PartyShareItemJudge sub_86DAF0, +114).
bool PartyServer::loot_shared(uint32_t owner, uint32_t picker)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(owner);
    return c && c->share.in_group && c->share.has(picker) && c->share.items;
}

// ── making and joining ───────────────────────────────────────────────────────

// PartyServer case 7001 -> sub_514110 -> PartyMember::MakeParty sub_50E0A0.
void PartyServer::make_party(uint32_t uid, const std::string& party_name, const std::string& candidate)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    LOG_INFO("PARTY SERVER   ON MAKE PARTY - makerUID : %u, partyName : %s, candidateName: '%s'", uid,
             party_name.c_str(), candidate.c_str());
    auto fail = [&](uint8_t code) {                    // sub_50E720
        PacketBuffer p = pkt(7003);
        p.write<uint8_t>(code);
        send(*c, p);
    };
    auto refuse = [&](const Client& who, uint8_t code) {   // sub_50E850
        PacketBuffer p = pkt(7015);
        write_str(p, who.info.name);
        p.write<uint8_t>(code);
        send(*c, p);
    };
    if (c->party) { fail(4); return; }
    if (asked(*c)) clear_asked(*c);
    if (c->info.level < kPartyLimitLevel) { fail(12); return; }
    Client* t = client_name(candidate);
    if (!t || t == c) { fail(3); return; }
    if (t->info.level < kPartyLimitLevel) { refuse(*t, 12); return; }
    if (asked(*t)) { refuse(*t, 5); return; }
    if (t->party) {
        // The one asked is in a party: S2C 7076 — ask to join that party instead (C2S 7077).
        Party* tp = party_of(*t);
        if (!tp || !has_master(*tp)) { refuse(*t, 5); return; }
        Client* tm = client_id(tp->master);
        PacketBuffer p = pkt(7076);
        p.write<uint16_t>(tp->id);
        write_str(p, t->info.name);
        write_str(p, tm ? tm->info.name : std::string());
        p.write<uint16_t>(static_cast<uint16_t>(avg_level(*tp)));
        party_type(*tp);
        p.write<uint8_t>(static_cast<uint8_t>(tp->ptype));
        p.write<uint8_t>(static_cast<uint8_t>(tp->members.size() + tp->dull.size()));
        p.write<uint8_t>(tp->division);
        p.write<uint8_t>(tp->items);
        send(*c, p);
        return;
    }

    uint16_t pid = next_party_;
    while (pid == 0 || parties_.count(pid)) ++pid;
    next_party_ = static_cast<uint16_t>(pid + 1);
    Party& pt = parties_[pid];
    pt.id = pid;
    pt.name = party_name.substr(0, kNameMax);
    c->asked = true;                                   // sub_50D3A0 on the maker
    c->asked_party = pid;
    c->asker = c->id;
    LOG_INFO("PARTY SERVER   PARTY - %u, ON MAKE PARTY", pid);   // sub_4F2250
    c->party = pid;
    pt.members.push_back(c->id);                       // sub_4F0C30
    pt.table.push_back(DivEntry{ c->id, 0 });
    pt.master = c->id;
    party_type(pt);
    pt.candidate = t->uid;                             // +640
    level_ok(pt, t->info.level);
    {
        PacketBuffer p = pkt(7051);
        p.write<uint16_t>(pid);
        p.write<uint16_t>(c->id);
        send(*c, p);
    }
    t->asked = true;
    t->asked_party = pid;
    t->asker = c->id;
    PacketBuffer p = pkt(7016);
    write_str(p, pt.name);
    write_str(p, c->info.name);
    p.write<uint8_t>(static_cast<uint8_t>(pt.members.size()));
    p.write<uint8_t>(pt.force_normal ? 0 : 3);
    p.write<uint8_t>(pt.items);
    p.write<uint16_t>(static_cast<uint16_t>(avg_level(pt)));
    send(*t, p);
}

// PartyMember case 7013 -> sub_50CBA0 (the name's character) -> AskParting sub_50DA60.
void PartyServer::ask_parting(uint32_t uid, const std::string& target)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Client* t = client_name(target);
    LOG_INFO("PARTY SERVER      PARTYMEMBER - %u, ON ASK PARTING - Target(%s)", uid, target.c_str());
    auto refuse = [&](uint8_t code) {                  // sub_50E850 (the name of the target's uid)
        PacketBuffer p = pkt(7015);
        write_str(p, t ? t->info.name : std::string());
        p.write<uint8_t>(code);
        send(*c, p);
    };
    Party* pt = party_of(*c);
    if (!pt) { refuse(2); return; }
    if (pt->master != c->id) { refuse(8); return; }
    if (full(*pt)) { refuse(9); return; }
    if (!t) { refuse(3); return; }
    if (t->party) { refuse(4); return; }
    if (!level_ok(*pt, t->info.level)) { refuse(12); return; }
    if (asked(*t)) { refuse(5); return; }
    t->asked = true;
    t->asked_party = pt->id;
    t->asker = c->id;
    PacketBuffer p = pkt(7016);
    write_str(p, pt->name);
    write_str(p, c->info.name);
    p.write<uint8_t>(static_cast<uint8_t>(pt->members.size()));
    p.write<uint8_t>(pt->division);
    p.write<uint8_t>(pt->items);
    p.write<uint16_t>(static_cast<uint16_t>(avg_level(*pt)));
    send(*t, p);
}

// PartyMember case 7017 -> AnswerPartingYes sub_50E930.
void PartyServer::answer_yes(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    LOG_INFO("PARTY SERVER   PARTYMEMBER - %u, ON ANSWER PARTING YES", uid);
    auto fail = [&](uint8_t code) {                    // sub_50ED60
        PacketBuffer p = pkt(7019);
        p.write<uint8_t>(code);
        send(*c, p);
    };
    if (!asked(*c)) { fail(6); return; }
    Party* ptp = party_id(c->asked_party);
    if (!ptp) { fail(10); return; }
    Party& pt = *ptp;
    Client* asker = client_id(c->asker);
    if (full(pt)) {
        fail(9);
        if (asker) {
            PacketBuffer p = pkt(7015);
            write_str(p, c->info.name);
            p.write<uint8_t>(9);
            send(*asker, p);
        }
        return;
    }
    if (asker) {
        if (pt.members.size() + pt.dull.size() < 2) {  // the first to join: the party is formed
            clear_asked(*asker);
            PacketBuffer p = pkt(7002);
            p.write<uint16_t>(pt.id);
            write_str(p, pt.name);
            p.write<uint16_t>(asker->id);
            send(*asker, p);
            board_remove_seeker(asker->uid);           // sub_50CCE0
        }
        PacketBuffer p = pkt(7014);                    // sub_50E7B0
        write_str(p, c->info.name);
        send(*asker, p);
    }
    clear_asked(*c);
    pt.candidate = 0;
    c->party = pt.id;
    add_member(pt, *c, false);
    board_remove_seeker(c->uid);                       // sub_50CCE0
    if (full(pt)) {
        board_remove_party(pt.id);                     // sub_50CD10
        if (has_master(pt))                            // the master hears the party is full
            if (Client* m = client_id(pt.master)) {
                PacketBuffer p = pkt(7071);
                p.write<uint32_t>(23);
                send(*m, p);
            }
    }
}

// PartyMember case 7020 [u8 why] -> AnswerPartingNo sub_50EE10; then an asker left alone leaves.
void PartyServer::answer_no(uint32_t uid, uint8_t reason)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    LOG_INFO("PARTY SERVER   PARTYMEMBER - %u, ON ANSWER PARTING NO (%u)", uid, reason);
    const uint16_t asker_id = c->asker;
    if (asked(*c)) {
        if (Client* a = client_id(asker_id)) {
            PacketBuffer p = pkt(7015);
            write_str(p, c->info.name);
            p.write<uint8_t>(reason);
            send(*a, p);
        }
        clear_asked(*c);
    }
    if (Client* a = client_id(asker_id))
        if (Party* pt = party_of(*a))
            if (pt->members.size() + pt->dull.size() < 2) leave_party(*a, true);
}

// sub_4F2F70: a new member (7033 to the others) or a dull one coming back (sub_50F6F0: 7045 to the
// others, then RECOVER DULL MEMBER sub_4F3E50); 7018 to it; the party type, the division.
void PartyServer::add_member(Party& pt, Client& c, bool rejoin)
{
    if (rejoin) {
        PacketBuffer p = pkt(7045);
        p.write<uint16_t>(c.id);
        p.write<uint64_t>(c.uid);
        p.write<uint16_t>(pt.force_normal ? 1 : 0);
        send_party(pt, p);
        LOG_INFO("PARTY SERVER   PARTY - %u, RECOVER DULL MEMBER - CHRUID : %u", pt.id, c.uid);
        pt.dull.erase(std::remove_if(pt.dull.begin(), pt.dull.end(), [&](const Dull& d) { return d.uid == c.uid; }),
                      pt.dull.end());
    } else {
        PacketBuffer p = pkt(7033);                    // to the members already there
        write_record(p, c);
        p.write<uint16_t>(pt.force_normal ? 1 : 0);
        send_party(pt, p);
    }
    LOG_INFO("PARTY SERVER   PARTY - %u, ADD MEMBER - CHRUID : %u", pt.id, c.uid);   // sub_4F0C30
    pt.members.push_back(c.id);
    pt.table.push_back(DivEntry{ c.id, 0 });           // sub_50BDC0(id, 1): a custom division becomes even
    if (pt.division == 2) pt.division = 1;
    board_update(pt);                                  // sub_50CD40
    send_party_info(pt, c);
    party_type(pt);                                    // sub_4F0E50
    observe(pt);                                       // (ours: the 2012 server leaves the observers as they
                                                       //  were until a zone change — the new member's bars
                                                       //  would stay empty)
    division(pt);                                      // sub_4F4D50
}

// S2C 7018: the whole party, to one member (c's member id as "me"), the dull members at the end.
void PartyServer::send_party_info(Party& pt, Client& c)
{
    std::vector<Client*> ms;
    for (uint16_t id : pt.members)
        if (Client* m = client_id(id)) ms.push_back(m);
    PacketBuffer p = pkt(7018);
    p.write<uint16_t>(pt.id);
    write_str(p, pt.name);
    p.write<uint16_t>(pt.master);
    p.write<uint16_t>(c.id);
    p.write<uint8_t>(pt.division);
    p.write<uint8_t>(static_cast<uint8_t>(ms.size()));
    p.write<uint16_t>(pt.force_normal ? 1 : 0);
    p.write<uint8_t>(pt.items);
    p.write<uint8_t>(pt.allow);
    for (Client* m : ms) write_record(p, *m);
    p.write<uint8_t>(static_cast<uint8_t>(pt.dull.size()));
    for (const Dull& d : pt.dull) {
        uint8_t r[60]{};
        std::memcpy(r, d.name.c_str(), std::min<size_t>(d.name.size(), 19));
        put16(r + 20, d.type);
        put16(r + 43, d.level);
        put64(r + 51, d.uid);
        p.write_bytes(r, sizeof(r));
    }
    send(c, p);
}

// sub_4F4D50: the division again after a change of members — a custom one (2) becomes by level (3); a
// Beginner party (9) is always by level, a Fixed one (10) each its own.
void PartyServer::division(Party& pt)
{
    uint8_t t = pt.division;
    if (t == 2) t = 3;
    if (pt.ptype == 9) t = 3;
    else if (pt.ptype == 10) t = 0;
    apply_division(pt, t, {});
}

// sub_4F2300: the division of type `type` (2: even shares, then the master's valid ones; 3: by level —
// each one's share of 100 by its level (floored), the remainder handed out +1 from the first one while
// the count of what is left still exceeds the index (so the first can get +2) — retail's loop kept);
// S2C 7039 to the party, then the share.
void PartyServer::apply_division(Party& pt, uint8_t type, const std::vector<DivEntry>& pcts)
{
    LOG_INFO("PARTY SERVER   PARTY - %u, ON CHANGE PARTY DIVISION - TYPE : %u", pt.id, type);
    pt.division = type;                                // sub_50C140
    if (type == 2 && !pt.table.empty()) {
        const int n = static_cast<int>(pt.table.size());
        const int q = 100 / n, r = 100 - n * q;
        for (int i = 0; i < n; ++i) pt.table[static_cast<size_t>(i)].pct = static_cast<uint8_t>(i ? q : q + r);
    }
    size_t count = 0;
    if (type == 3) {
        std::vector<Client*> ms;
        for (uint16_t id : pt.members)
            if (Client* m = client_id(id)) ms.push_back(m);
        count = pt.members.size();
        int sum = 0;
        for (Client* m : ms) sum += m->info.level;
        int remainder = 100;
        for (Client* m : ms) {
            const int v = sum > 0 ? static_cast<int>(std::floor(static_cast<double>(m->info.level) /
                                                                static_cast<double>(sum) * 100.0)) : 0;
            auto e = std::find_if(pt.table.begin(), pt.table.end(), [&](const DivEntry& x) { return x.id == m->id; });
            if (e == pt.table.end()) {
                LOG_ERROR("divisionsCnt:%zu — PTYMEMBERID:%u missing (PartyExpBug)", pt.table.size(), m->id);
                continue;
            }
            e->pct = static_cast<uint8_t>(v);
            remainder -= v;
        }
        while (remainder > 0 && !ms.empty())
            for (int k = 0; k < remainder && k < static_cast<int>(ms.size()); ++k) {
                for (DivEntry& e : pt.table)
                    if (e.id == ms[static_cast<size_t>(k)]->id) e.pct = static_cast<uint8_t>(e.pct + 1);
                --remainder;
            }
    } else if (type == 2) {
        count = pcts.size();
        const size_t n = std::min<size_t>(pt.table.size(), 6);
        for (const DivEntry& in : pcts) {
            if (std::find(pt.members.begin(), pt.members.end(), in.id) == pt.members.end()) continue;   // sub_4F37E0
            for (DivEntry& e : pt.table)                                                                 // sub_50C1E0
                if (e.id == in.id && in.pct >= kDivMin[n] && in.pct <= kDivMax[n]) e.pct = in.pct;      // sub_50C240
        }
    }
    PacketBuffer p = pkt(7039);
    p.write<uint8_t>(type);
    if (type == 2 || type == 3) {
        p.write<uint8_t>(static_cast<uint8_t>(count));
        for (size_t i = 0; i < count; ++i) {
            const DivEntry e = i < pt.table.size() ? pt.table[i] : DivEntry{};
            p.write<uint16_t>(e.id);
            p.write<uint8_t>(e.pct);
        }
    } else {
        p.write<uint8_t>(0);
    }
    send_party(pt, p);
    order_share(pt);                                   // sub_4F0780
    board_update(pt);                                  // sub_50CD40
}

// ── leaving ──────────────────────────────────────────────────────────────────

// sub_4F1240: out of the list and the division table; the master's place goes to the first member left.
void PartyServer::remove_member(Party& pt, Client& c)
{
    LOG_INFO("PARTY SERVER   PARTY - %u, SUB MEMBER - CHRUID : %u", pt.id, c.uid);
    pt.members.erase(std::remove(pt.members.begin(), pt.members.end(), c.id), pt.members.end());
    pt.table.erase(std::remove_if(pt.table.begin(), pt.table.end(), [&](const DivEntry& e) { return e.id == c.id; }),
                   pt.table.end());
    if (c.id == pt.master) pt.master = pt.members.empty() ? 0 : pt.members.front();
    party_type(pt);
    clear_share(c);
    if (c.type_bonus) {
        c.type_bonus = 0;
        if (c.sid) gs_.post_to_session(c.sid, [](GameClientSession& s) { s.party_type_bonus(0); });
    }
}

// sub_4F1420: S2C 7052 to everyone still in it; its dull members leave the depot.
void PartyServer::disband(Party& pt)
{
    LOG_INFO("PARTY SERVER   DELETE PARTY - ID : %u", pt.id);
    if (pt.pq.qid) {                                   // its party quest ends: msg 18 reason 1 (party_quest.cpp)
        pq_notify_cancel(pt, 1);
        pt.pq = Party::PqRun{};
    }
    board_remove_party(pt.id);                         // (sub_50CD10 before every sub_4F1400)
    for (const Dull& d : pt.dull) depot_.erase(d.uid);
    for (uint16_t id : pt.members)
        if (Client* m = client_id(id)) {
            m->party = 0;
            send(*m, pkt(7052));
            clear_share(*m);
            if (m->type_bonus) {
                m->type_bonus = 0;
                if (m->sid) gs_.post_to_session(m->sid, [](GameClientSession& s) { s.party_type_bonus(0); });
            }
        }
    for (auto& kv : clients_)                          // the invitations to it lapse
        if (kv.second.asked && kv.second.asked_party == pt.id) clear_asked(kv.second);
    parties_.erase(pt.id);
}

// PartyMember case 7021 -> sub_50EEB0 (+ sub_4F36C0): out, S2C 7034 to the rest; below 2 members (the
// dull ones not counted) the party breaks up.
void PartyServer::leave_party(Client& c, bool online)
{
    Party* pt = party_of(c);
    if (!pt) return;
    remove_member(*pt, c);
    {
        PacketBuffer p = pkt(7034);
        p.write<uint16_t>(c.id);
        p.write<uint16_t>(pt->master);
        send_party(*pt, p);
    }
    c.party = 0;
    if (pt->members.size() >= 2) {
        party_type(*pt);
        division(*pt);                                 // (sub_4F0780 + sub_4F4D50: the division's own share)
    } else {
        disband(*pt);
    }
    if (online) send(c, pkt(7021));
}

void PartyServer::leave(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (c) leave_party(*c, true);
}

// sub_50F5E0 (ON LEAVE PARTY TEMPORARILY): a master alone takes its party down; anyone else goes dull
// (sub_4F3A10) and into the depot (AddDullPartyClient sub_514A20 -> PushIntoDepot sub_510470).
void PartyServer::leave_temporarily(Client& c)
{
    Party* pt = party_of(c);
    if (!pt) return;
    LOG_INFO("PARTY SERVER   PARTYMEMBER - %u, ON LEAVE PARTY TEMPORARILY", c.uid);
    if (pt->master == c.id && pt->members.size() == 1) {
        remove_member(*pt, c);
        c.party = 0;
        disband(*pt);
        return;
    }
    if (!depot_.count(c.uid)) depot_[c.uid] = Reserved{ pt->id, depot_period_ };
    else LOG_ERROR("PARTY  PartyMemberDepot::PushIntoDepot:AlreadIn Depot:%u:", c.uid);
    make_dull(*pt, c);
    c.party = 0;
}

// sub_4F3A10 -> sub_4F3B10 (MAKE DULL MEMBER): kept as {name, uid, level, type}, out of the members and
// the division (a master's place goes to the first member: sub_4F2C40), S2C 7046 to the rest.
void PartyServer::make_dull(Party& pt, Client& c)
{
    LOG_INFO("PARTY SERVER   PARTY - %u, ON LEAVE TEMP - CHRUID : %u", pt.id, c.uid);
    LOG_INFO("PARTY SERVER   PARTY - %u, MAKE DULL MEMBER - CHRUID : %u", pt.id, c.uid);
    pt.dull.push_back(Dull{ c.info.name, c.uid, static_cast<uint16_t>(c.info.level), c.info.type });
    pt.members.erase(std::remove(pt.members.begin(), pt.members.end(), c.id), pt.members.end());
    pt.table.erase(std::remove_if(pt.table.begin(), pt.table.end(), [&](const DivEntry& e) { return e.id == c.id; }),
                   pt.table.end());
    clear_share(c);
    c.type_bonus = 0;
    if (c.id == pt.master && !pt.members.empty()) {
        change_master_to(pt, pt.members.front());
    } else {
        party_type(pt);
        order_share(pt);
    }
    PacketBuffer p = pkt(7046);
    p.write<uint16_t>(c.id);
    p.write<uint16_t>(pt.master);
    send_party(pt, p);
}

// PartyMember case 7025 [u16 id] -> sub_50F1F0 (-> sub_4F2A50).
void PartyServer::kick(uint32_t uid, uint16_t member)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    LOG_INFO("PARTY SERVER   PARTYMEMBER - %u, ON FORCED SUB PARTY MEMBER - ID : %u", uid, member);
    auto fail = [&](uint8_t code) {                    // sub_50F360
        PacketBuffer p = pkt(7027);
        p.write<uint8_t>(code);
        send(*c, p);
    };
    Party* pt = party_of(*c);
    if (!pt) { fail(2); return; }
    if (pt->master != c->id) { fail(8); return; }
    Client* t = client_id(member);
    if (!t || std::find(pt->members.begin(), pt->members.end(), member) == pt->members.end() || member == pt->master) {
        fail(11);
        return;
    }
    PacketBuffer p = pkt(7037);
    p.write<uint16_t>(member);
    remove_member(*pt, *t);
    send_party(*pt, p);
    send(*t, p);
    t->party = 0;
    const bool forced = pt->force_normal;              // sub_4F2A50: the master's level again; a party that
    if (Client* m = client_id(pt->master)) level_ok(*pt, m->info.level);   // leaves the force-normal mode
    if (forced && !pt->force_normal) pt->division = 3; // goes by level (sub_50C140(3))
    division(*pt);                                     // (sub_4F4D50, before the count)
    if (pt->members.size() >= 2) send(*c, pkt(7026));
    else disband(*pt);
}

// PartyMember case 7010 [u16 id] -> sub_50F410 -> sub_4F2C40.
void PartyServer::change_master(uint32_t uid, uint16_t member)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    auto fail = [&](uint8_t code) {                    // sub_50F4F0
        PacketBuffer p = pkt(7012);
        p.write<uint8_t>(code);
        send(*c, p);
    };
    Party* pt = party_of(*c);
    if (!pt) { fail(2); return; }
    if (pt->master != c->id) { fail(8); return; }
    if (std::find(pt->members.begin(), pt->members.end(), member) == pt->members.end()) { fail(2); return; }
    change_master_to(*pt, member);
    // (retail then sends S2C 7011 to the old master — the 2014 client has no case for it: not sent)
}

// sub_4F2C40: requests welcome again (+618 = 1), the new master's level (force normal), the division,
// then S2C 7044 [master][force normal].
void PartyServer::change_master_to(Party& pt, uint16_t member)
{
    LOG_INFO("PARTY SERVER   PARTY - %u, ON CHANAGE PARTY MASTER TO %u", pt.id, member);
    if (member == pt.master || std::find(pt.members.begin(), pt.members.end(), member) == pt.members.end()) return;
    Client* old = has_master(pt) ? client_id(pt.master) : nullptr;   // (a dull old master: none)
    pt.allow = 1;
    pt.master = member;
    Client* m = client_id(member);
    if (m) level_ok(pt, m->info.level);
    PacketBuffer p = pkt(7044);
    p.write<uint16_t>(member);
    p.write<uint8_t>(pt.force_normal ? 1 : 0);
    party_type(pt);
    division(pt);
    send_party(pt, p);
    board_update(pt);                                  // sub_50CD40
    for (BoardParty& r : board_parties_)               // sub_50CD70 -> TossMasterBallon (sub_4FD480)
        if (r.party == pt.id) {
            if (old) balloon(old->uid, 0, std::string());
            if (m) { r.balloon = m->uid; balloon(m->uid, 1, r.comment); }
        }
}

// PartyMember case 7042 [str text][u16 (unread)] -> sub_50E6E0 -> sub_4F2190: S2C 7043 [u16 who][str
// text] to the whole party, the speaker too (client case 0x1B83).
void PartyServer::chat(uint32_t uid, const std::string& text)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Party* pt = party_of(*c);
    if (!pt) return;
    std::string line = text;
    cut_client_text(line, kClientChatMax);   // Hardening (not retail): the client's chat-box limit (game_server.hpp)
    PacketBuffer p = pkt(7043);
    p.write<uint16_t>(c->id);
    write_str(p, line);
    send_party(*pt, p);
}

// ── the master's settings ────────────────────────────────────────────────────

// PartyMember case 7004 [u8 type]([u8 n][n x (u16 who, u8 %)] when 2) -> sub_50EF70.
void PartyServer::change_division(uint32_t uid, uint8_t type, const std::vector<DivEntry>& pcts)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    LOG_INFO("PARTY SERVER   PARTYMEMBER - %u, ON CHANGE PARTY DIVISION - TYPE : %u", uid, type);
    auto fail = [&](uint8_t code) {                    // sub_50F000
        LOG_INFO("PARTY SERVER   PARTYMEMBER - %u, FAILED ON CHANGE PARTY DIVISION - ERROR : %u", uid, code);
        PacketBuffer p = pkt(7006);
        p.write<uint8_t>(code);
        send(*c, p);
    };
    Party* pt = party_of(*c);
    if (!pt) { fail(2); return; }
    if (pt->master != c->id) { fail(8); return; }
    apply_division(*pt, type, pcts);
    send(*c, pkt(7005));
}

// PartyMember case 7007 [str name] -> sub_50F0B0 -> sub_4F2990: S2C 7038 [str name] to the party.
void PartyServer::rename(uint32_t uid, const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    LOG_INFO("PARTY SERVER   PARTYMEMBER - %u, ON CHANGE NAME - NAME : %s", uid, name.c_str());
    auto fail = [&](uint8_t code) {                    // sub_50F140
        PacketBuffer p = pkt(7009);
        p.write<uint8_t>(code);
        send(*c, p);
    };
    Party* pt = party_of(*c);
    if (!pt) { fail(2); return; }
    if (pt->master != c->id) { fail(8); return; }
    pt->name = name.substr(0, kNameMax);
    LOG_INFO("PARTY SERVER   PARTY - %u, ON CHANGE NAME - NAME : %s", pt->id, pt->name.c_str());
    PacketBuffer p = pkt(7038);
    write_str(p, pt->name);
    send_party(*pt, p);
    send(*c, pkt(7008));
}

// PartyMember case 7054 [u8] (the master) -> sub_4F4BB0: a new mode goes out as S2C 7056, then the share
// again; anyone else gets 7055.
void PartyServer::item_mode(uint32_t uid, bool on)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Party* pt = party_of(*c);
    if (!pt || pt->master != c->id) {
        send(*c, pkt(7055));
        return;
    }
    const uint8_t mode = on ? 1 : 0;
    if (pt->items != mode) {
        pt->items = mode;
        PacketBuffer p = pkt(7056);
        p.write<uint8_t>(mode);
        send_party(*pt, p);
    }
    order_share(*pt);
}

// PartyMember case 7079 [u8] (any member) -> sub_50F980 -> sub_4F4B20: S2C 7078 [u8] to the party.
void PartyServer::allow_requests(uint32_t uid, bool on)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Party* pt = party_of(*c);
    if (!pt) return;
    pt->allow = on ? 1 : 0;
    PacketBuffer p = pkt(7078);
    p.write<uint8_t>(pt->allow);
    send_party(*pt, p);
}

// PartyMember case 7047 [u64 uid] (the master) -> sub_50F720 -> sub_4F3FA0 (OnForcedSubPartyDullMember):
// S2C 7048 [uid] to the party (7049 to the master when there is no such dull member); the master's
// level, the division; below 2 members the party breaks up.
void PartyServer::kick_dull(uint32_t uid, uint64_t dull_uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Party* pt = party_of(*c);
    if (!pt) return;
    if (pt->master != c->id) {
        LOG_INFO("PARTY PartyMember::OnForcedSubPartyDullMember(): OnlyMasterCan: %llu:!",
                 static_cast<unsigned long long>(dull_uid));
        return;
    }
    LOG_INFO("PARTY PartyMember::OnForcedSubPartyDullMember(): ForcedSubDull: %llu:(start)!",
             static_cast<unsigned long long>(dull_uid));
    auto d = std::find_if(pt->dull.begin(), pt->dull.end(), [&](const Dull& x) { return x.uid == dull_uid; });
    if (d != pt->dull.end()) {
        pt->dull.erase(d);
        depot_.erase(static_cast<uint32_t>(dull_uid));
        PacketBuffer p = pkt(7048);
        p.write<uint64_t>(dull_uid);
        send_party(*pt, p);
    } else {
        LOG_INFO("PARTY Party::OnForcedSubPartyDullMember: PARTY(%u),CHRUID(%llu),IsInUIDList(0)", pt->id,
                 static_cast<unsigned long long>(dull_uid));
        PacketBuffer p = pkt(7049);
        p.write<uint64_t>(dull_uid);
        send_master(*pt, p);
    }
    if (Client* m = client_id(pt->master)) level_ok(*pt, m->info.level);
    party_type(*pt);
    division(*pt);
    if (pt->members.size() < 2) disband(*pt);
}

// ── joining by request ───────────────────────────────────────────────────────

// sub_4F4A20 + sub_4F4A70: one request at a time (+624); S2C 7066 [str who][u16][u32] to the master.
void PartyServer::join_request(Client& asker, Party& pt, const std::string& me, uint16_t lv, uint32_t x)
{
    auto fail = [&](uint32_t code) {                   // sub_514690
        PacketBuffer p = pkt(7068);
        p.write<uint32_t>(code);
        send(asker, p);
    };
    if (full(pt)) { fail(9); return; }
    Client* who = client_name(me);
    if (!who) { fail(0); return; }
    if (pt.pending) { fail(18); return; }
    pt.pending = who->uid;
    if (!has_master(pt)) { fail(20); return; }
    PacketBuffer p = pkt(7066);
    write_str(p, me);
    p.write<uint16_t>(lv);
    p.write<uint32_t>(x);
    send_master(pt, p);
}

// PartyServer case 7066 [u16 party][str me][u16][u32] -> sub_514210 (a request by party id, the board's).
void PartyServer::request_by_id(uint32_t uid, uint16_t party, const std::string& me, uint16_t lv, uint32_t x)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Party* pt = party_id(party);
    if (!pt) {
        PacketBuffer p = pkt(7068);
        p.write<uint32_t>(20);
        send(*c, p);
        return;
    }
    join_request(*c, *pt, me, lv, x);
}

// PartyServer case 7077 [str member][str me][u16][u32] -> sub_514340 (a request to the party of a member).
void PartyServer::request_join(uint32_t uid, const std::string& member, const std::string& me, uint16_t lv,
                               uint32_t x)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Client* m = client_name(member);
    Party* pt = m ? party_of(*m) : nullptr;
    if (!pt) {
        PacketBuffer p = pkt(7068);
        p.write<uint32_t>(m ? 0 : 20);
        send(*c, p);
        return;
    }
    join_request(*c, *pt, me, lv, x);
}

// PartyServer case 7080 [str name] -> sub_5144A0: the party of that character, S2C 7081 — else 7082.
void PartyServer::lookup_party(uint32_t uid, const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Client* m = client_name(name);
    Party* pt = m ? party_of(*m) : nullptr;
    if (!pt || full(*pt)) {
        PacketBuffer p = pkt(7082);
        p.write<uint32_t>(pt ? 9 : 0);
        send(*c, p);
        return;
    }
    party_type(*pt);
    PacketBuffer p = pkt(7081);
    p.write<uint16_t>(pt->id);
    write_str(p, name);
    p.write<uint16_t>(static_cast<uint16_t>(avg_level(*pt)));
    p.write<uint8_t>(static_cast<uint8_t>(pt->ptype));
    p.write<uint8_t>(static_cast<uint8_t>(pt->members.size()));
    p.write<uint8_t>(pt->division);
    p.write<uint8_t>(pt->items);
    send(*c, p);
}

// PartyMember case 7069 (the master) -> sub_50DD60: the pending request joins — S2C 7014 to the master,
// the asker is added (7033 / 7018 / 7039); a full party then says so (7071 [23]). Failures: 7071 [u32].
void PartyServer::accept_request(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    auto fail = [&](uint32_t code) {                   // sub_50E020
        PacketBuffer p = pkt(7071);
        p.write<uint32_t>(code);
        send(*c, p);
    };
    Party* pt = party_of(*c);
    if (!pt || pt->master != c->id) { fail(0); return; }
    if (full(*pt)) { fail(9); return; }
    if (!pt->pending) { fail(0); return; }
    Client* who = client(pt->pending);
    if (!who) { fail(11); return; }
    if (who->party) { fail(19); return; }
    if (asked(*who)) { fail(0); return; }
    who->asked = true;                                 // sub_50D3A0
    who->asked_party = pt->id;
    who->asker = c->id;
    if (!level_ok(*pt, who->info.level)) {
        clear_asked(*who);
        fail(0);
        return;
    }
    {
        PacketBuffer p = pkt(7014);                    // sub_50E7B0
        write_str(p, who->info.name);
        send(*c, p);
    }
    clear_asked(*who);
    who->party = pt->id;
    add_member(*pt, *who, false);
    pt->pending = 0;
    if (full(*pt)) {
        board_remove_party(pt->id);                    // sub_50CD10
        fail(23);
    }
    board_remove_seeker(who->uid);                     // sub_50CCE0
}

// PartyMember case 7072 (the master) -> sub_50F820: S2C 7072 to the asker. (Not the master: retail
// answers 7074 — the 2014 client has no case for it: not sent.)
void PartyServer::deny_request(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Party* pt = party_of(*c);
    if (!pt || pt->master != c->id) return;
    send_uid(pt->pending, pkt(7072));                  // sub_50CDE0 (when online)
    pt->pending = 0;
}

// PartyMember case 7075 (the master) -> sub_50F910: the party breaks up (sub_4F1420).
void PartyServer::dissolve(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Party* pt = party_of(*c);
    if (!pt || pt->master != c->id) return;
    disband(*pt);
}

// PartyMember case 7083 -> sub_50FA50: the maker takes its invitation back (+640) — S2C 7083 to the one
// asked (Alert_RequestCanceled), whose invitation lapses.
void PartyServer::cancel_invite(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Party* pt = party_of(*c);
    if (!pt) return;
    const uint32_t cand = pt->candidate;
    pt->candidate = 0;
    Client* t = cand ? client(cand) : nullptr;
    if (!t) return;
    send(*t, pkt(7083));
    if (asked(*t)) clear_asked(*t);
}

// PartyMember cases 7058..7061 (a member in a party; sub_4F4430 / sub_4F44D0 / sub_4F45B0 / sub_4F4650 ->
// sub_4F4730: the others): S2C 7062 / 7063 / 7064 / 7065 [u16 1 / 0 / 1 / 0][u64 the member] (client sub_89D270 /
// sub_89D2C0: the member record's byte +49 / +50, the window redrawn). 7059 / 7061 then run OnNotifyEnterZone
// (Party vtbl+8 sub_4F1DE0) with the member's zone. (The client sends 7058 leaving a scene, sub_9FFFA0.)
void PartyServer::member_status(uint32_t uid, uint16_t op)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    Party* pt = c ? party_of(*c) : nullptr;
    if (!pt) return;
    PacketBuffer p = pkt(static_cast<uint16_t>(op + 4));
    p.write<uint16_t>((op == 7058 || op == 7060) ? 1 : 0);
    p.write<uint64_t>(c->uid);
    for (uint16_t id : pt->members)
        if (id != c->id)
            if (Client* m = client_id(id)) send(*m, p);
    if (op == 7059 || op == 7061) {                    // sub_4F1DE0
        party_type(*pt);
        PacketBuffer z = pkt(7036);
        z.write<uint16_t>(c->id);
        z.write<uint16_t>(static_cast<uint16_t>(c->info.zone));
        send_party(*pt, z);
        observe(*pt);
        order_share(*pt);
        if (pt->pq.qid) pq_notify(*pt);
    }
}

// ── the party board ──────────────────────────────────────────────────────────
//  C2S 7302 [u16 page][u8 sort][u8 forward][u8 filter][u16 my level] / 7305 [u16 page][u8 sort][u8 forward]
//  the lists (8 a page) -> S2C 7303 / 7306 [u8 n][n x 72-byte record]; 7308 / 7311 [str comment] post a
//  party / a seeker -> 7309 / 7312 (client "Alert_BoardWritten"); 7314 / 7317 take it down -> 7315 /
//  7318; 7320 / 7321 [u16 page][u8 sort][u8 forward][u8 kind][str] search (0 a name, 1 / 2 a level
//  band) -> 7303 / 7306; 7322 [u16 party] -> 7323 [str master][u16 lv][u32 zone][u8 n][u8 division]
//  [u8 item mode][str comment] (else 7324); 7325 [u64 uid] -> 7326 [str name][u32 zone][str comment]
//  (else 7327); errors 7301 [u32] (9 full, 13 in a party, 14 master only, 15 posted, 16 board full,
//  17 not in a party). A post shows its comment over its owner's head (the master of a party): the
//  GameServer's PartyBoardHeadNotice -> S2C 470 / 471 (party_board_balloon).

namespace {

constexpr size_t kBoardParties = 1000;   // PartyBoard +256 (this[64])
constexpr size_t kBoardSeekers = 1500;   // +1120 (this[280])
constexpr size_t kBoardFilter  = 24;     // the filters' result list (sub_503A80)

std::string cut(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }

} // namespace

// sub_4FDAD0: S2C 7301 [u32 code] (client sub_89B220).
void PartyServer::board_error(const Client& c, uint32_t code)
{
    PacketBuffer p = pkt(7301);
    p.write<uint32_t>(code);
    send(c, p);
}

// msg 212 -> the GameServer's PartyBoardHeadNotice (sub_86D760 -> sub_86D6D0): the balloon over a
// character's head (type 1 a party's post on its master, 2 a seeker's own; 0 takes it down).
void PartyServer::balloon(uint32_t uid, uint8_t type, const std::string& text)
{
    Client* c = client(uid);
    if (!c || !c->sid) return;
    gs_.post_to_session(c->sid, [type, text](GameClientSession& s) { s.party_board_balloon(type, text); });
}

// sub_4FA040 & co: a page of 8 (from 8 x page), forward or from the end.
void PartyServer::board_send_parties(const Client& c, const std::vector<const BoardParty*>& v, uint16_t page,
                                     bool forward)
{
    const size_t total = v.size();
    const size_t start = std::min<size_t>(8u * page, total);
    const size_t n = std::min<size_t>(8, total - start);
    PacketBuffer p = pkt(7303);
    p.write<uint8_t>(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const BoardParty& r = *(forward ? v[start + i] : v[total - 1 - start - i]);
        uint8_t b[72]{};
        put16(b, r.party);
        put32(b + 4, r.master_type);
        put32(b + 8, r.ptype);
        put16(b + 12, r.avg_level);
        std::memcpy(b + 14, r.master_name.c_str(), std::min<size_t>(r.master_name.size(), 19));
        std::memcpy(b + 34, r.comment.c_str(), std::min<size_t>(r.comment.size(), 34));
        p.write_bytes(b, sizeof(b));
    }
    send(c, p);
}

void PartyServer::board_send_seekers(const Client& c, const std::vector<const BoardSeeker*>& v, uint16_t page,
                                     bool forward)
{
    const size_t total = v.size();
    const size_t start = std::min<size_t>(8u * page, total);
    const size_t n = std::min<size_t>(8, total - start);
    PacketBuffer p = pkt(7306);
    p.write<uint8_t>(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const BoardSeeker& r = *(forward ? v[start + i] : v[total - 1 - start - i]);
        uint8_t b[72]{};
        put64(b, r.uid);
        put32(b + 8, r.type);
        put16(b + 12, r.level);
        std::memcpy(b + 14, r.name.c_str(), std::min<size_t>(r.name.size(), 19));
        std::memcpy(b + 34, r.comment.c_str(), std::min<size_t>(r.comment.size(), 34));
        p.write_bytes(b, sizeof(b));
    }
    send(c, p);
}

// The board's orders (a sphList per key, filled at the post — a later update of the record keeps its
// place): 0 the posting order, 1 by party type, 4 by average level; seekers 0 / 7 the posting order, 8
// by level.
namespace {
template <class R, class K>
std::vector<const R*> board_order(const std::vector<R>& all, K key)
{
    std::vector<const R*> v;
    for (const R& r : all) v.push_back(&r);
    std::stable_sort(v.begin(), v.end(), [&](const R* a, const R* b) { return key(*a) < key(*b); });
    return v;
}
} // namespace

// PartyBoard case 7302 -> sub_4FC370: the parties seeking members (a filter: the first 24 within 3 levels of
// ours, sub_503A80, then sub_4FAF30).
void PartyServer::board_list_parties(uint32_t uid, uint16_t page, uint8_t sort, bool forward, bool filter,
                                     uint16_t level)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    std::vector<const BoardParty*> v;
    if (sort == 0) for (const BoardParty& r : board_parties_) v.push_back(&r);
    else if (sort == 1) v = board_order(board_parties_, [](const BoardParty& r) { return r.key_type; });
    else if (sort == 4) v = board_order(board_parties_, [](const BoardParty& r) { return r.key_level; });
    else if (!filter) return;
    if (filter) {
        std::vector<const BoardParty*> f;
        for (const BoardParty* r : v) {
            if (static_cast<int>(level) >= r->avg_level - 3 && static_cast<int>(level) <= r->avg_level + 3) f.push_back(r);
            if (f.size() >= kBoardFilter) break;
        }
        board_send_parties(*c, f, page, forward);
        return;
    }
    board_send_parties(*c, v, page, sort == 0 ? false : forward);   // (sub_4FA040: the newest first)
}

// PartyBoard case 7305 -> sub_4FC420: the characters seeking a party.
void PartyServer::board_list_seekers(uint32_t uid, uint16_t page, uint8_t sort, bool forward)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    std::vector<const BoardSeeker*> v;
    if (sort == 0 || sort == 7) {
        for (const BoardSeeker& r : board_seekers_) v.push_back(&r);
        board_send_seekers(*c, v, page, false);                     // sub_4FB610: the newest first
    } else if (sort == 8) {
        board_send_seekers(*c, board_order(board_seekers_, [](const BoardSeeker& r) { return r.level; }), page, forward);
    }
}

// PartyBoard case 7308 -> sub_4FC630 (OnPostMemberSeek): a party member posts its party.
void PartyServer::board_post_party(uint32_t uid, const std::string& comment)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    LOG_INFO("PARTY SERVER   BOARD - PartyBoard::OnPostMemberSeek - CHRUID : %u, Comment : %s", uid, comment.c_str());
    Party* pt = party_of(*c);
    if (!pt) { board_error(*c, 17); return; }
    if (full(*pt)) { board_error(*c, 9); return; }
    for (const BoardParty& r : board_parties_) if (r.party == pt->id) { board_error(*c, 15); return; }
    if (board_parties_.size() >= kBoardParties) { board_error(*c, 16); return; }
    Client* m = client_id(pt->master);
    party_type(*pt);
    BoardParty r;                                      // sub_4FE4A0
    r.party       = pt->id;
    r.master_type = m ? m->info.type : 0;
    r.master_name = m ? cut(m->info.name, 19) : std::string();
    r.ptype       = static_cast<uint32_t>(pt->ptype);
    r.count       = static_cast<uint8_t>(pt->members.size() + pt->dull.size());
    r.avg_level   = static_cast<uint16_t>(avg_level(*pt));
    r.division    = pt->division;
    r.items       = pt->items;
    r.comment     = cut(comment, 34);
    r.key_type    = r.ptype;
    r.key_level   = r.avg_level;
    r.balloon     = m ? m->uid : 0;
    board_parties_.push_back(r);
    if (m) balloon(m->uid, 1, r.comment);
    send(*c, pkt(7309));
}

// PartyBoard case 7311 -> sub_4FC950 (OnPostPartySeek): a character without a party posts itself.
void PartyServer::board_post_seeker(uint32_t uid, const std::string& comment)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    LOG_INFO("PARTY SERVER   BOARD - PartyBoard::OnPostPartySeek - CHRUID : %u, Comment : %s", uid, comment.c_str());
    if (c->party) { board_error(*c, 13); return; }
    for (const BoardSeeker& r : board_seekers_) if (r.uid == uid) { board_error(*c, 15); return; }
    if (board_seekers_.size() >= kBoardSeekers) { board_error(*c, 16); return; }
    BoardSeeker r;                                     // sub_4FE620
    r.uid     = uid;
    r.type    = c->info.type;
    r.level   = static_cast<uint16_t>(c->info.level);
    r.name    = cut(c->info.name, 19);
    r.comment = cut(comment, 34);
    board_seekers_.push_back(r);
    balloon(uid, 2, r.comment);
    send(*c, pkt(7312));
}

// PartyBoard case 7314 -> sub_4FCC00 (OnDeleteMemberSeek): the master takes its party's post down.
void PartyServer::board_delete_party(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Party* pt = party_of(*c);
    if (!pt) return;                                   // (retail 7316: no client case — not sent)
    if (pt->master != c->id) { board_error(*c, 14); return; }
    for (const BoardParty& r : board_parties_)
        if (r.party == pt->id) {
            board_remove_party(pt->id);
            send(*c, pkt(7315));
            return;
        }
}

// PartyBoard case 7317 -> sub_4FCD70 (OnDeletePartySeek).
void PartyServer::board_delete_seeker(uint32_t uid)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    for (const BoardSeeker& r : board_seekers_)
        if (r.uid == uid) {
            board_remove_seeker(uid);
            send(*c, pkt(7318));
            return;
        }
}

// PartyBoard case 7320 -> sub_4FC4A0: a master's name (sub_4FB460: the name it was posted under), or the
// first 24 parties of average level in [v, v + 3] (kind 1, sub_503F90) / [v - 3, v] (kind 2, sub_504470).
void PartyServer::board_search_parties(uint32_t uid, uint16_t page, uint8_t sort, bool forward, uint8_t kind,
                                       const std::string& text)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    if (kind == 0) {
        std::vector<const BoardParty*> v;
        const std::string name = cut(text, 19);
        for (const BoardParty& r : board_parties_)
            if (r.master_name == name) { v.push_back(&r); break; }
        board_send_parties(*c, v, 0, true);
        return;
    }
    if (kind != 1 && kind != 2) return;
    const int lv = std::atoi(text.c_str());
    std::vector<const BoardParty*> v;
    if (sort == 0) for (const BoardParty& r : board_parties_) v.push_back(&r);
    else if (sort == 1) v = board_order(board_parties_, [](const BoardParty& r) { return r.key_type; });
    else if (sort == 4) v = board_order(board_parties_, [](const BoardParty& r) { return r.key_level; });
    std::vector<const BoardParty*> f;
    for (const BoardParty* r : v) {
        const int a = r->avg_level;
        if (kind == 1 ? (lv <= a && lv >= a - 3) : (lv >= a && lv <= a + 3)) f.push_back(r);
        if (f.size() >= kBoardFilter) break;
    }
    board_send_parties(*c, f, page, forward);
}

// PartyBoard case 7321 -> sub_4FC560: the same for the seekers (sub_4FC220 / sub_504950 / sub_504E50).
void PartyServer::board_search_seekers(uint32_t uid, uint16_t page, uint8_t sort, bool forward, uint8_t kind,
                                       const std::string& text)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    if (kind == 0) {
        std::vector<const BoardSeeker*> v;
        const std::string name = cut(text, 19);
        for (const BoardSeeker& r : board_seekers_)
            if (r.name == name) { v.push_back(&r); break; }
        board_send_seekers(*c, v, 0, true);
        return;
    }
    if (kind != 1 && kind != 2) return;
    const int lv = std::atoi(text.c_str());
    std::vector<const BoardSeeker*> v;
    if (sort == 0 || sort == 7) for (const BoardSeeker& r : board_seekers_) v.push_back(&r);
    else if (sort == 8) v = board_order(board_seekers_, [](const BoardSeeker& r) { return r.level; });
    std::vector<const BoardSeeker*> f;
    for (const BoardSeeker* r : v) {
        const int a = r->level;
        if (kind == 1 ? (lv <= a && lv >= a - 3) : (lv >= a && lv <= a + 3)) f.push_back(r);
        if (f.size() >= kBoardFilter) break;
    }
    board_send_seekers(*c, f, page, forward);
}

// PartyBoard case 7322 [u16 party] -> sub_4FDB50: S2C 7323 (the master now, the post's numbers), else 7324.
void PartyServer::board_party_info(uint32_t uid, uint16_t party)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    const BoardParty* r = nullptr;
    for (const BoardParty& x : board_parties_) if (x.party == party) r = &x;
    Party* pt = party_id(party);
    Client* m = (pt && has_master(*pt)) ? client_id(pt->master) : nullptr;
    if (!r || !m) {
        send(*c, pkt(7324));
        return;
    }
    PacketBuffer p = pkt(7323);
    write_str(p, m->info.name);
    p.write<uint16_t>(static_cast<uint16_t>(m->info.level));
    p.write<uint32_t>(static_cast<uint32_t>(m->moving ? 0 : m->info.zone));
    p.write<uint8_t>(r->count);
    p.write<uint8_t>(r->division);
    p.write<uint8_t>(r->items);
    write_str(p, r->comment);
    send(*c, p);
}

// PartyBoard case 7325 [u64 uid] -> sub_4FDDB0 (OnRequestCandiInfo): S2C 7326 [str name][u32 zone][str
// comment] for an online seeker, else 7327.
void PartyServer::board_seeker_info(uint32_t uid, uint64_t who)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Client* c = client(uid);
    if (!c) return;
    Client* w = client(static_cast<uint32_t>(who));
    const BoardSeeker* r = nullptr;
    for (const BoardSeeker& x : board_seekers_) if (x.uid == who) r = &x;
    if (!w || !w->sid || !r) {
        send(*c, pkt(7327));
        return;
    }
    PacketBuffer p = pkt(7326);
    write_str(p, w->info.name);
    p.write<uint32_t>(static_cast<uint32_t>(w->moving ? 0 : w->info.zone));
    write_str(p, r->comment);
    send(*c, p);
}

// sub_4FD260 (UpdateParty): a posted party's record follows its master and numbers (its places in the
// sorted lists stay); a party with no members or no master leaves the board.
void PartyServer::board_update(Party& pt)
{
    for (BoardParty& r : board_parties_) {
        if (r.party != pt.id) continue;
        Client* m = has_master(pt) ? client_id(pt.master) : nullptr;
        if (pt.members.empty() || !m) {
            board_remove_party(pt.id);
            return;
        }
        r.master_type = m->info.type;
        r.master_name = cut(m->info.name, 19);
        r.ptype       = static_cast<uint32_t>(pt.ptype);
        r.count       = static_cast<uint8_t>(pt.members.size() + pt.dull.size());
        r.avg_level   = static_cast<uint16_t>(avg_level(pt));
        r.division    = pt.division;
        r.items       = pt.items;
        return;                                        // (the balloon moves only with sub_4F2C40's toss)
    }
}

// sub_4FCF30: the party's post goes, and the balloon over its master.
void PartyServer::board_remove_party(uint16_t party)
{
    for (size_t i = 0; i < board_parties_.size(); ++i)
        if (board_parties_[i].party == party) {
            const uint32_t b = board_parties_[i].balloon;
            board_parties_.erase(board_parties_.begin() + static_cast<std::ptrdiff_t>(i));
            if (b) balloon(b, 0, std::string());
            return;
        }
}

// sub_4FCE90: a seeker's post goes (it joined a party, logged out, or took it down), and its balloon.
void PartyServer::board_remove_seeker(uint32_t uid)
{
    for (size_t i = 0; i < board_seekers_.size(); ++i)
        if (board_seekers_[i].uid == uid) {
            board_seekers_.erase(board_seekers_.begin() + static_cast<std::ptrdiff_t>(i));
            balloon(uid, 0, std::string());
            return;
        }
}

// ── the session side ─────────────────────────────────────────────────────────

namespace {

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

} // namespace

PartyInfo GameClientSession::party_info() const
{
    PartyInfo i;
    i.name   = char_name_;
    i.type   = static_cast<uint16_t>(type_);
    i.zone   = zone_id_;
    i.hp     = hp_;
    i.mp     = mp_;
    i.max_hp = hp_max_;
    i.max_mp = mp_max_;
    i.level  = level_;
    i.x      = static_cast<int16_t>(pos_x_);
    i.y      = static_cast<int16_t>(pos_y_);
    return i;
}

// A party / party board command of character uid (C2S 7001..7088, 7302..7325) — from its community
// connection (system_session.cpp), as the client sends them, or from the game connection.
void party_dispatch(PartyServer& ps, uint32_t uid, PacketBuffer& pkt, uint16_t op)
{
    try {
        switch (op) {
        case 7001: { const std::string name = read_cstr(pkt); const std::string cand = read_cstr(pkt);
                     ps.make_party(uid, name, cand); break; }
        case 7004: {
            const uint8_t type = pkt.read<uint8_t>();
            std::vector<PartyServer::DivEntry> v;
            if (type == 2) {
                const uint8_t n = pkt.read<uint8_t>();
                for (uint8_t i = 0; i < n; ++i) {
                    PartyServer::DivEntry e;
                    e.id  = pkt.read<uint16_t>();
                    e.pct = pkt.read<uint8_t>();
                    v.push_back(e);
                }
            }
            ps.change_division(uid, type, v);
            break;
        }
        case 7007: ps.rename(uid, read_cstr(pkt)); break;
        case 7010: ps.change_master(uid, pkt.read<uint16_t>()); break;
        case 7013: ps.ask_parting(uid, read_cstr(pkt)); break;
        case 7017: ps.answer_yes(uid); break;
        case 7020: ps.answer_no(uid, pkt.read<uint8_t>()); break;
        case 7021: ps.leave(uid); break;
        case 7025: ps.kick(uid, pkt.read<uint16_t>()); break;
        case 7042: ps.chat(uid, read_cstr(pkt)); break;
        case 7047: ps.kick_dull(uid, pkt.read<uint64_t>()); break;
        case 7054: ps.item_mode(uid, pkt.read<uint8_t>() != 0); break;
        case 7066: {
            const uint16_t party = pkt.read<uint16_t>();
            std::string me = read_cstr(pkt);
            cut_client_text(me, kClientChatMax);   // Hardening (not retail): relayed to the master (S2C 7066)
            const uint16_t lv = pkt.read<uint16_t>();
            const uint32_t x  = pkt.read<uint32_t>();
            ps.request_by_id(uid, party, me, lv, x);
            break;
        }
        case 7069: ps.accept_request(uid); break;
        case 7072: ps.deny_request(uid); break;
        case 7075: ps.dissolve(uid); break;
        case 7077: {
            const std::string member = read_cstr(pkt);
            std::string me = read_cstr(pkt);
            cut_client_text(me, kClientChatMax);   // Hardening (not retail): relayed to the master (S2C 7066)
            const uint16_t lv = pkt.read<uint16_t>();
            const uint32_t x  = pkt.read<uint32_t>();
            ps.request_join(uid, member, me, lv, x);
            break;
        }
        case 7079: ps.allow_requests(uid, pkt.read<uint8_t>() != 0); break;
        case 7080: ps.lookup_party(uid, read_cstr(pkt)); break;
        case 7083: ps.cancel_invite(uid); break;
        case 7058: case 7059: case 7060: case 7061: ps.member_status(uid, op); break;
        case 7088: ps.pq_remove(uid); break;              // the master stops the party quest (party_quest.cpp)
        // the party board (client sub_981C40 / sub_9C2B00 / sub_981D90 / sub_9C2BF0 / sub_97FD90 /
        // sub_9C1260 / sub_981E40 / sub_9C2CE0 / sub_9B65C0)
        case 7302: {
            const uint16_t page = pkt.read<uint16_t>();
            const uint8_t sort = pkt.read<uint8_t>();
            const bool fwd = pkt.read<uint8_t>() != 0;
            const bool filter = pkt.read<uint8_t>() != 0;
            const uint16_t lv = pkt.read<uint16_t>();
            ps.board_list_parties(uid, page, sort, fwd, filter, lv);
            break;
        }
        case 7305: {
            const uint16_t page = pkt.read<uint16_t>();
            const uint8_t sort = pkt.read<uint8_t>();
            const bool fwd = pkt.read<uint8_t>() != 0;
            ps.board_list_seekers(uid, page, sort, fwd);
            break;
        }
        case 7308: ps.board_post_party(uid, read_cstr(pkt)); break;
        case 7311: ps.board_post_seeker(uid, read_cstr(pkt)); break;
        case 7314: ps.board_delete_party(uid); break;
        case 7317: ps.board_delete_seeker(uid); break;
        case 7320:
        case 7321: {
            const uint16_t page = pkt.read<uint16_t>();
            const uint8_t sort = pkt.read<uint8_t>();
            const bool fwd = pkt.read<uint8_t>() != 0;
            const uint8_t kind = pkt.read<uint8_t>();
            const std::string text = read_cstr(pkt);
            if (op == 7320) ps.board_search_parties(uid, page, sort, fwd, kind, text);
            else            ps.board_search_seekers(uid, page, sort, fwd, kind, text);
            break;
        }
        case 7322: ps.board_party_info(uid, pkt.read<uint16_t>()); break;
        case 7325: ps.board_seeker_info(uid, pkt.read<uint64_t>()); break;
        default: break;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("party C2S %u (chr %u) malformed: %s", op, uid, e.what());
    }
}

// Party commands that come on the game connection (the client sends them on its community one).
void GameClientSession::handle_party(PacketBuffer& pkt, uint16_t op)
{
    party_dispatch(server_.party(), char_uid_, pkt, op);
}

void GameClientSession::server_party_closed()
{
    if (char_uid_) server_.party().closed(char_uid_, session_id());
}

// The PartyTypeBonusProcessor (sub_86D480 -> sub_86D3B0, released by sub_86D230): 1 = the
// PartyRecoveryTimeAccelator (the HP / MP recovery comes after 0.8 of its time, regen_tick); 2..5 = the
// PartyStatusPtsAdder (+4 accumulated points to the three stats of line bonus - 2, sub_86D550) — that one
// and any release recount the stats (sub_7164C0(1)) and send the points (S2C 0x006D, sub_5F4FF0).
void GameClientSession::party_type_bonus(uint8_t bonus)
{
    if (bonus == party_bonus_) return;
    const uint8_t old = party_bonus_;
    party_bonus_ = bonus;
    LOG_INFO("GameClientSession %u: party type bonus %u -> %u", session_id(), old, bonus);
    if (bonus >= 2 || old != 0) {
        recompute_stats(true);
        send_points();
    }
}

// The PartyBoardHeadNotice on us (Player sub_73CC20 / sub_73CC70): the board comment over our head —
// state flag 7, type (+3464) and text (+3468, 50 chars) -> S2C 470 [u16 us][u8 type][str text] to the
// area (sub_5F89A0); taken down -> S2C 471 [u16 us] (sub_5F8A40). Client sub_6F0960 cases 470 / 471 ->
// the player actor's sub_6C9450 / sub_6C8820; our appear carries it too (strMask bit 8).
void GameClientSession::party_board_balloon(uint8_t type, const std::string& text)
{
    if (type) {
        board_type_ = type;
        board_text_ = text.substr(0, 50);
        PacketBuffer p(static_cast<Opcode>(470), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<uint8_t>(type);
        p.write_bytes(board_text_.c_str(), board_text_.size() + 1);
        server_.broadcast_zone(zone_id_, p, 0);
    } else if (board_type_) {
        board_type_ = 0;
        board_text_.clear();
        PacketBuffer p(static_cast<Opcode>(471), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        server_.broadcast_zone(zone_id_, p, 0);
    }
}

// sub_717170: a stat's accumulated points with the party's adder.
int GameClientSession::acc_points(int i) const
{
    if (i < 0 || i >= 12) return 0;
    int v = accpts_[i];
    if (party_bonus_ >= 2 && party_bonus_ <= 5 && i / 3 == party_bonus_ - 2) v += 4;
    return v;
}

// S2C 0x006D (sub_5F4FF0): [u16 unspent][12 x u16 accumulated points] (the adder's included).
void GameClientSession::send_points()
{
    PacketBuffer p(static_cast<Opcode>(0x006D), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(attr_pts_));
    for (int i = 0; i < 12; ++i) p.write<uint16_t>(static_cast<uint16_t>(acc_points(i)));
    send(p);
}

// Every 500 ms (the observer's tick, sub_86C400): the party sees our numbers (S2C 7040, what changed).
void GameClientSession::party_tick(int elapsed_ms)
{
    party_sync_ms_ -= elapsed_ms;
    if (party_sync_ms_ > 0) return;
    party_sync_ms_ = 500;
    server_.party().update(char_uid_, party_info());
}
