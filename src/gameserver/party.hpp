#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "packet.hpp"

class cGameServer;

// ─────────────────────────────────────────────────────────────────────────────
//  PartyServer — the retail SystemServer's party (SystemServer\Party\Party.cpp: PartyServer::OnCmd
//  sub_513EC0, PartyMember::OnCmd sub_50D460, Party, PartyMemberDepot), run inside the GameServer process:
//  here one game server holds every player. The client talks to it on its game connection (sub_89B050 ->
//  sub_83D570, allowed once S2C 5506 opened the community session) with C2S 7001..7088 and reads the
//  answers in dispatch_char_7000 (sub_89F450).
//
//  A logged-in character is a PartyMember with a member id (its key in the client's party list).
//  A field change here is a reconnection (S2C 154 -> C2S 156 on a new connection): the member is
//  kept by its character uid — leave_zone() marks it moving (S2C 7035), the next session takes it
//  back (S2C 7036) — the retail Party::OnNotifyEnterZone. A disconnection leaves the party
//  "temporarily" (sub_50F5E0): the member becomes dull (S2C 7046) and waits in the PartyMemberDepot
//  for PartyMemberDepotInterval; logging in again brings it back (S2C 7045), the depot's end drops it
//  (S2C 7050).
//
//  The GameServer half (PartyShareExpProcessor, PartyObserveAndNotifyProcessor, ...) is here too: the
//  share groups each member reads for its EXP / TM / loot (share_of) and the members' S2C 7040.
// ─────────────────────────────────────────────────────────────────────────────

// A member's record as the client draws it (the 60-byte party record, client sub_B20280).
struct PartyInfo {
    std::string name;
    uint16_t    type{ 0 };                 // +20 the illustration (the model shown, 1..28)
    int16_t     zone{ 0 };                 // +23 (0: on the move)
    int32_t     hp{ 0 }, mp{ 0 };          // +27 / +31
    int32_t     max_hp{ 0 }, max_mp{ 0 };  // +35 / +39
    int16_t     level{ 0 };                // +43
    int16_t     x{ 0 }, y{ 0 };            // the position (S2C 7040 sends it / 16)
};

// A member's place in its share group — the GameServer's PartyShareExpProcessor (msg 12 from the
// SystemServer's PartyExpShareVisitor, sub_518D60 -> sub_86CAC0): the same-zone, living, online members
// of a party, their EXP / TM percentages and the party bonus, set on each member's rates (Player +5352).
struct PartyShare {
    struct Slot { uint32_t uid{ 0 }; float exp_pct{ 1.0f }; float tm_pct{ 1.0f }; };
    bool     in_group{ false };     // a processor holds us (any party member in a zone)
    bool     on{ false };           // +113 the EXP / TM go out by the percentages (else each its own)
    bool     items{ false };        // +114 the party's item mode (S2C 7056): members pick up each other's loot
    float    exp_rate{ 0.0f };      // rate slot 0 (+5372): the party EXP bonus - 1 + 0.005
    float    tm_rate{ 0.0f };       // TM rate slot 0 (+5436): the party TM bonus - 1 + 0.005
    float    exp_pct{ 1.0f };       // rates +12: min(our EXP percentage * 0.01, 1)
    float    tm_pct{ 1.0f };        // rates +16: min(our TM percentage * 0.01, 1)
    std::vector<Slot> members;      // the group's 6 slots (us included)
    bool has(uint32_t uid) const
    {
        for (const Slot& s : members) if (s.uid == uid) return true;
        return false;
    }
};

// A party quest as a member's GameServer keeps it (CPartyQuestInventory, sub_7DB880): the one entry (+24, 32
// bytes: sub_7DAB20) and its timer (+0..+23: sub_7DB030 / sub_7DB1E0). party_quest.cpp.
struct PartyQuestState {
    int      qid{ 0 };                  // +8 (0: none)
    uint64_t uid{ 0 };                  // +0 the PartyQuest row
    uint16_t monster{ 0 };              // +12
    uint16_t kills{ 0 };                // +14 ours
    uint16_t target{ 0 };               // +16
    uint8_t  members{ 0 };              // +18
    int32_t  record{ 0 };               // +20 the time left when it was done
    bool     done{ false };             // +24
    uint16_t com{ 0 };                  // +26 the times done before (the DB)
    int32_t  limit_ms{ 0 };             // timer +0
    int64_t  start_ms{ 0 };             // timer +12 (the clock / 10000 at the start)
    int32_t  left_ms{ 0 };              // timer +16
    int32_t  acc_ms{ 0 };               // timer +20 (the 1-second steps)
    uint16_t total{ 0 };                // timer +4 the party's kills
    bool active() const { return qid && !done && left_ms > 0; }   // sub_7DBAF0
};
// A kill record of the party quest's update (msg 21: 6 character uids + their kills).
struct PqRec { uint32_t uid{ 0 }; uint16_t kills{ 0 }; };

class PartyServer;
// A party / party board command (C2S 7001..7088, 7302..7325) of character uid, from either connection.
void party_dispatch(PartyServer& ps, uint32_t uid, PacketBuffer& pkt, uint16_t op);

class PartyServer {
public:
    explicit PartyServer(cGameServer& gs) : gs_(gs) {}

    // The character's session: a login or the re-entry after a field change (PartyMember register).
    void enter(uint32_t uid, uint32_t sid, const PartyInfo& info, bool paula);
    // A field change began (S2C 154 sent): the member is on the move (S2C 7035 to its party).
    void leave_zone(uint32_t uid, int16_t dest_zone);
    // The session closed: unless another session took the member back, it leaves "temporarily".
    void closed(uint32_t uid, uint32_t sid);
    // The member's numbers now (every 500 ms): S2C 7040 to its same-zone mates with what changed.
    void update(uint32_t uid, const PartyInfo& info);
    // Party::OnNotifyDead (sub_4F4C70) / the rebirth (sub_4F4CB0): out of / back into the share.
    void dead(uint32_t uid);
    void revived(uint32_t uid);
    // The PartyMemberDepot's clock (the dull members' time).
    void tick(int32_t elapsed_ms);
    // The character's community connection (system_session.cpp): the party's own packets go that way.
    void attach_system(uint32_t uid, uint32_t sys_sid);
    void detach_system(uint32_t uid, uint32_t sys_sid);

    // The client's commands.
    void make_party(uint32_t uid, const std::string& party_name, const std::string& candidate);   // C2S 7001
    void ask_parting(uint32_t uid, const std::string& target);                                    // C2S 7013
    void answer_yes(uint32_t uid);                                                                // C2S 7017
    void answer_no(uint32_t uid, uint8_t reason);                                                 // C2S 7020
    void leave(uint32_t uid);                                                                     // C2S 7021
    void kick(uint32_t uid, uint16_t member);                                                     // C2S 7025
    void change_master(uint32_t uid, uint16_t member);                                            // C2S 7010
    void chat(uint32_t uid, const std::string& text);                                             // C2S 7042
    struct DivEntry { uint16_t id{ 0 }; uint8_t pct{ 0 }; };
    void change_division(uint32_t uid, uint8_t type, const std::vector<DivEntry>& pcts);          // C2S 7004
    void rename(uint32_t uid, const std::string& name);                                           // C2S 7007
    void item_mode(uint32_t uid, bool on);                                                        // C2S 7054
    void allow_requests(uint32_t uid, bool on);                                                   // C2S 7079
    void kick_dull(uint32_t uid, uint64_t dull_uid);                                              // C2S 7047
    void request_by_id(uint32_t uid, uint16_t party, const std::string& me, uint16_t lv, uint32_t x);   // C2S 7066
    void request_join(uint32_t uid, const std::string& member, const std::string& me, uint16_t lv,
                      uint32_t x);                                                                // C2S 7077
    void lookup_party(uint32_t uid, const std::string& name);                                     // C2S 7080
    void accept_request(uint32_t uid);                                                            // C2S 7069
    void deny_request(uint32_t uid);                                                              // C2S 7072
    void dissolve(uint32_t uid);                                                                  // C2S 7075
    void cancel_invite(uint32_t uid);                                                             // C2S 7083
    void member_status(uint32_t uid, uint16_t op);                                                // C2S 7058..7061
    // The PartyBoard (SystemServer PartyBoard.cpp, sub_4FD780): parties seeking members, characters
    // seeking a party.
    void board_list_parties(uint32_t uid, uint16_t page, uint8_t sort, bool forward, bool filter,
                            uint16_t level);                                                      // C2S 7302
    void board_list_seekers(uint32_t uid, uint16_t page, uint8_t sort, bool forward);             // C2S 7305
    void board_post_party(uint32_t uid, const std::string& comment);                              // C2S 7308
    void board_post_seeker(uint32_t uid, const std::string& comment);                             // C2S 7311
    void board_delete_party(uint32_t uid);                                                        // C2S 7314
    void board_delete_seeker(uint32_t uid);                                                       // C2S 7317
    void board_search_parties(uint32_t uid, uint16_t page, uint8_t sort, bool forward, uint8_t kind,
                              const std::string& text);                                           // C2S 7320
    void board_search_seekers(uint32_t uid, uint16_t page, uint8_t sort, bool forward, uint8_t kind,
                              const std::string& text);                                           // C2S 7321
    void board_party_info(uint32_t uid, uint16_t party);                                          // C2S 7322
    void board_seeker_info(uint32_t uid, uint64_t who);                                           // C2S 7325

    // Our place in our share group (a copy; in_group false when none).
    PartyShare share_of(uint32_t uid);
    // The sessions of uid's share group (the same-zone, living, online members) but uid's own.
    std::vector<uint32_t> group_sessions(uint32_t uid);
    // The PartyRewardHandler (sub_86DC40 / sub_86CCD0): the EXP / TM a member earned goes out to its
    // group (each one's session runs party_exp / party_tm). False: the earner adds it the usual way.
    bool reward(uint32_t uid, int32_t amount, uint8_t flag, bool tm);
    // PartyEnemyJudge / PartyMemberJudge: `other` is in uid's share group.
    bool same_group(uint32_t uid, uint32_t other);
    // An owned item `picker` may pick up: the owner's group holds it and shares items (+114).
    bool loot_shared(uint32_t owner, uint32_t picker);

    // The party quests (the SystemServer's CPartyQuestMonster at Party +648, and the GameServer's view of the
    // party, CPartyManager — msg 16). party_quest.cpp has the members' side.
    struct PqView {
        uint16_t party{ 0 };            // CPartyManager +8 (0: no party)
        uint32_t master{ 0 };           // +0 the master's character
        std::vector<std::pair<uint32_t, uint32_t>> members;   // (character, its session or 0), in party order
    };
    bool pq_view(uint32_t uid, PqView& out);
    // SYSCMD 5553 -> PartyServer::BeginPartyQuest (sub_514E50 -> sub_4F4DE0 -> CPartyQuestMonster::Initialize
    // sub_511CE0): false when the party already plays one (or is gone).
    bool pq_begin(uint32_t uid, uint64_t pq_uid, int qid, uint16_t monster, uint16_t target, int64_t start,
                  int32_t limit, const std::vector<std::pair<uint32_t, uint8_t>>& coms);
    void pq_kill(uint32_t uid, uint16_t monster);   // SYSCMD 5557 -> sub_515150 -> Party::PartyQuestKillMonster
    void pq_cancel(uint32_t uid, int qid);          // SYSCMD 5589 -> sub_515260 -> Party::CancelPartyQuest
    void pq_remove(uint32_t uid);                   // C2S 7088 -> PartyMember::OnCmdRemovePartyQuest (sub_50F9C0)

private:
    struct Client {                     // PartyMember
        uint16_t  id{ 0 };              // +10
        uint32_t  uid{ 0 };
        uint32_t  sid{ 0 };             // its session (0: none)
        bool      moving{ false };
        int32_t   moving_ms{ 0 };       // on the move with no session yet (a field change's reconnect)
        bool      dead{ false };        // +36
        PartyInfo info;
        PartyInfo sent;                 // what its observers saw last (S2C 7040)
        uint16_t  party{ 0 };           // +16
        bool      asked{ false };       // +24 a pending invitation (or the maker's own, while making)
        uint16_t  asked_party{ 0 };     // +26
        uint16_t  asker{ 0 };           // +28 the member who asked
        PartyShare share;               // the processor holding it (the GameServer side)
        uint8_t   type_bonus{ 0 };      // its PartyTypeBonusProcessor: 1 recovery, 2..5 a line's stats
    };
    struct Dull {                       // sPartyDullMember
        std::string name;
        uint32_t    uid{ 0 };
        uint16_t    level{ 0 };
        uint16_t    type{ 0 };
    };
    struct Party {
        uint16_t              id{ 0 };            // +8
        std::string           name;               // +10 (36 bytes)
        uint16_t              master{ 0 };        // +46 the master's member id
        std::vector<uint16_t> members;            // in the order they came (+96, +196 the count)
        std::vector<Dull>     dull;               // +584 the count
        std::vector<DivEntry> table;              // +52 the division entries (+88 the count)
        uint8_t               division{ 3 };      // +92 the EXP division type
        bool                  force_normal{ false };   // +616
        uint8_t               items{ 0 };         // +617 the item mode
        uint8_t               allow{ 0 };         // +618 join requests are welcome
        uint32_t              pending{ 0 };       // +624 the character asking to join
        uint32_t              candidate{ 0 };     // +640 the one the maker asked
        int                   ptype{ 0 };         // +632 the party type (PartyType.cpp: 0..10)
        struct PqKill { uint32_t uid{ 0 }; uint16_t kills{ 0 }; uint8_t com{ 0 }; };   // sKillRecord
        struct PqRun {                            // +648 CPartyQuestMonster
            int      qid{ 0 };                    // +0 (0: none, sub_5124F0)
            uint64_t uid{ 0 };                    // +8
            uint16_t monster{ 0 };                // +16
            uint16_t target{ 0 };                 // +18
            uint16_t total{ 0 };                  // +20 (done: total == target, sub_512510)
            int64_t  start{ 0 };                  // +24
            int32_t  limit{ 0 };                  // +28
            std::vector<PqKill> recs;             // the members at the start, by character
        } pq;
    };
    struct Reserved {                   // sPartyReservedMember (PartyMemberDepot)
        uint16_t party{ 0 };
        uint32_t period{ 0 };
    };
    struct BoardParty {                 // MemberSeekerRecord (the first 72 bytes go out)
        uint16_t    party{ 0 };         // +0
        uint32_t    master_type{ 0 };   // +4
        uint32_t    ptype{ 0 };         // +8
        uint16_t    avg_level{ 0 };     // +12
        std::string master_name;        // +14 [20]
        std::string comment;            // +34 [35]
        uint8_t     count{ 0 }, division{ 0 }, items{ 0 };   // +72 / +73 / +74
        uint32_t    key_type{ 0 };      // its places in the sorted lists (fixed at the post)
        uint16_t    key_level{ 0 };
        uint32_t    balloon{ 0 };       // the character whose head shows it (the master)
    };
    struct BoardSeeker {                // PartySeekerRecord
        uint32_t    uid{ 0 };           // +0 (u64)
        uint32_t    type{ 0 };          // +8
        uint16_t    level{ 0 };         // +12
        std::string name;               // +14 [20]
        std::string comment;            // +34 [35]
    };

    Client* client(uint32_t uid);
    Client* client_id(uint16_t id);
    Client* client_name(const std::string& name);
    Party*  party_of(const Client& c);
    Party*  party_id(uint16_t id);
    bool    asked(Client& c);                     // sub_50D150
    void    clear_asked(Client& c);               // sub_50D430
    int     avg_level(const Party& p);            // sub_4F4850
    bool    level_ok(Party& p, int level);        // sub_4EFDF0
    bool    full(const Party& p) const { return p.members.size() + p.dull.size() >= 6; }   // sub_4EFDB0
    bool    has_master(const Party& p) const;     // sub_4EFC00

    PacketBuffer pkt(uint16_t op) const { return PacketBuffer(static_cast<Opcode>(op), paula_); }
    void send(const Client& c, const PacketBuffer& p);          // a SystemServer packet: the community link
    void send_game(const Client& c, const PacketBuffer& p);     // a GameServer one (886 / 887 / 7040)
    void send_uid(uint32_t uid, const PacketBuffer& p);
    void send_party(const Party& pt, const PacketBuffer& p, uint16_t except = 0);
    void send_master(const Party& pt, const PacketBuffer& p);   // sub_4F4220
    void write_record(PacketBuffer& p, const Client& c) const;

    void add_member(Party& pt, Client& c, bool rejoin);     // sub_4F2F70
    void send_party_info(Party& pt, Client& c);       // S2C 7018
    void remove_member(Party& pt, Client& c);         // sub_4F1240 (+ sub_4F36C0's S2C 7034)
    int  party_type(const Party& pt);                 // sub_4F0E50 -> sub_518210
    void division(Party& pt);                         // sub_4F4D50 -> sub_4F2300
    void apply_division(Party& pt, uint8_t type, const std::vector<DivEntry>& pcts);   // sub_4F2300
    void order_share(Party& pt);                      // sub_4F0780 -> sub_518D60
    void clear_share(Client& c);                      // a released processor (sub_86C860)
    void send_rates(const Client& c);                 // S2C 886 / 887 (sub_8880B0)
    void observe(Party& pt);                          // sub_4EFE40: the observers start over
    void disband(Party& pt);                          // sub_4F1420: S2C 7052
    void leave_party(Client& c, bool online);         // sub_50EEB0
    void leave_temporarily(Client& c);                // sub_50F5E0
    void drop(Client& c);                             // UnregisterCharacter (sub_513E10)
    void make_dull(Party& pt, Client& c);             // sub_4F3A10 -> sub_4F3B10
    void delete_dull(uint32_t uid, uint16_t party);   // sub_514940 (the depot's end)
    void change_master_to(Party& pt, uint16_t member);   // sub_4F2C40
    void join_request(Client& asker, Party& pt, const std::string& me, uint16_t lv, uint32_t x);  // sub_4F4A20/70
    void board_error(const Client& c, uint32_t code);  // sub_4FDAD0: S2C 7301
    void board_update(Party& pt);                     // sub_4FD260 (UpdateParty)
    void board_remove_party(uint16_t party);          // sub_4FCF30
    void board_remove_seeker(uint32_t uid);           // sub_4FCE90
    void balloon(uint32_t uid, uint8_t type, const std::string& text);   // msg 212 (PartyBoardHeadNotice)
    void pq_notify(Party& pt);                        // sub_4F04E0: msg 21 to every member (PartyOrderUpdateQuest)
    void pq_notify_cancel(Party& pt, uint32_t why);   // sub_4F51C0 -> sub_4F5280: msg 18 (PartyOrderCancelQuest)
    void pq_entered(Party& pt, Client& c);            // OnNotifyEnterZone / a login back: the member's quest view
    void board_send_parties(const Client& c, const std::vector<const BoardParty*>& v, uint16_t page, bool forward);
    void board_send_seekers(const Client& c, const std::vector<const BoardSeeker*>& v, uint16_t page, bool forward);

    cGameServer&                           gs_;
    std::recursive_mutex                   mutex_;
    bool                                   paula_{ false };
    std::unordered_map<uint16_t, Client>   clients_;     // by member id
    std::unordered_map<uint32_t, uint16_t> by_uid_;      // character uid -> member id
    std::unordered_map<uint16_t, Party>    parties_;     // by party id
    std::unordered_map<uint32_t, Reserved> depot_;       // the dull characters (uid -> their party)
    std::unordered_map<uint32_t, uint32_t> systems_;     // character uid -> its community session
    std::vector<BoardParty>                board_parties_;   // in posting order (at most 1000)
    std::vector<BoardSeeker>               board_seekers_;   // in posting order (at most 1500)
    int32_t                                depot_ms_{ 0 };
    uint32_t                               depot_period_{ 0 };
    uint16_t                               next_member_{ 1 };
    uint16_t                               next_party_{ 1 };
};
