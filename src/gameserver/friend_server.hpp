#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "packet.hpp"

class cGameServer;

// ─────────────────────────────────────────────────────────────────────────────
//  FriendServer — the retail SystemServer's friend module (SystemServer\Friend: FriendServer::OnCmd sub_46DC80, the
//  Friend object + its command queue sub_458C90 and FriendXxxCmd classes) on the community connection
//  (system_session.cpp). The commands run at once here (the retail queues them per character and waits on the DB).
//  C2S (client friend module, sender wrapper sub_853100): 8501 login (empty), 8502 logout, 8503 [str] ask a friend,
//  8504 [str] drop an online friend, 8505 [str] drop an offline one, 8506 [u64 asker][u8][str] yes, 8507 [u64 asker]
//  no, 8519 [str] cancel my request, 8538 [u64][u8 old][u32 new] move a friend's group, 8541 [u32 group][str] rename
//  a group, 8543 [u16] take requests (dead in the client), 8575 [u64 asker] the asked one can't answer now.
//  S2C (VERIFIED, client dispatch_friend_msg_8500 sub_85A050): 8510 the list at login (8537 without friends) [u32 n]
//  {str dropped-me name} [u32 5]{u32 group, str name} ([u32 n]{u64 uid, u8 group, str name, u8 online, u16 type})
//  [u32 n]{u64 sender, str date, str text, str name, u32 kind}; 8511 / 8512 [u64] a friend logged in / out; 8516
//  [u64 asker][str name] the request (to the asked one); 8548 sent; 8514 [u64][u8 group][str][u8 1][u16 type] /
//  8518 [u64][u8][str][u16] a new friend (the asker / the acceptor); 8515 [str] refused; 8547 you refused; 8520
//  [str] cancelled; 8531 / 8532 [u64][str] dropped (the dropped one / the dropper), 8533 [u64][str] dropped offline,
//  8534 dropping (busy); 8539 / 8540 [u64][u8 old][u32 new] moved / refused; 8576 [str] the asked one can't answer;
//  messages: 8508 [u8 code], 8509 no such character, 8513 not online, 8525 yourself, 8526 busy, 8529 already,
//  8542 / 8550 60 friends, 8544 takes none, 8545 gone, 8567 / 8580 failed, 8583 DB failed.
//  Notes (the messenger): 8556 [u64 to][str] -> 8559 [u64 from][str] to it + 8560 [u64 to] to us (8557 not online, 8558
//  not logged in, 8565 not our friend, 8566 we are not its friend; its client may answer C2S 8573 [u64 from] -> 8574 to
//  the sender: it takes no notes); 8561 [u64 to][str] offline -> usp_insert_friend_sms (100 a character) -> 8563 / 8564.
//  A friend's position: 8551 [u64 uid] -> 8552 [u16 zone] / 8553 [u16 from][u16 to] (between maps) / 8545 (not logged
//  in), and in our map 8554 [u16 x / 16][u16 y / 16][u16 zone][u64 uid] on our game connection; going to a friend:
//  8586 [u64 uid] -> 8587 [u16 x][u16 y][u16 zone] (the client's Friend Finder, Effect 39, takes it) / 8588 [u8 0
//  between maps, 1 the same map, 2 not reachable] / 8513 (not logged in).
//  Not done: the "office" checks on the target's game server (agency MsgID 181: treated as accepted), 8549 / 8521 /
//  8522; the couple server's note hook (sub_449370).
// ─────────────────────────────────────────────────────────────────────────────
class FriendServer
{
public:
    explicit FriendServer(cGameServer& srv) : srv_(srv) {}

    void login(uint32_t sys_sid, uint32_t char_uid, bool paula);     // C2S 8501 (FriendLoginCmd)
    void logout(uint32_t sys_sid);                                    // 8502 / the connection closes (FriendLogOutCmd)
    void command(uint32_t sys_sid, PacketBuffer& pkt, uint16_t op, bool paula);

private:
    struct Entry { uint32_t uid{ 0 }; uint8_t group{ 0 }; std::string name; uint16_t type{ 0 }; };
    struct Client {
        uint32_t sid{ 0 }, uid{ 0 };
        std::string name;
        uint16_t type{ 0 };
        bool paula{ true };
        bool logged{ false };            // +184: the login is complete
        bool busy{ false };              // +192: a request in progress
        uint16_t accept{ 1 };            // +186: takes requests (sub_457D90 = 1)
        std::string pending;             // +152: the asker's name (on the asked one)
        std::vector<Entry> friends;      // +648 the list
        std::string groups[5];
    };

    Client* by_sid(uint32_t sid);
    Client* by_uid(uint32_t uid);
    Client* by_name(const std::string& name);
    static bool has(const Client& c, uint32_t uid);
    void send(const Client& c, uint16_t op, const PacketBuffer* body = nullptr);
    void send_raw(const Client& c, PacketBuffer& p);
    static std::string key(const std::string& name);

    void ask(Client& me, const std::string& name);                  // 8503: Request + Office + RequestAdd
    void yes(Client& me, uint32_t asker);                             // 8506: FriendAddYesCmd
    void no(Client& me, uint32_t asker);                              // 8507: FriendAddNoCmd
    void cancel(Client& me, const std::string& name);                // 8519: FriendAddCancelCmd
    void drop(Client& me, const std::string& name);                  // 8504: FriendDelCmd
    void drop_offline(Client& me, const std::string& name);          // 8505: FriendDelOfflineCmd
    void move(Client& me, uint32_t uid, uint8_t old_group, uint32_t group);   // 8538: FriendUpdateCmd
    void rename(Client& me, uint32_t group, const std::string& name);         // 8541: FriendUpdateGroupCmd
    void cannot(Client& me, uint32_t asker);                          // 8575: the asked one can't answer
    void note(Client& me, uint32_t to, const std::string& text);      // 8556: OnCmdSendSMS (sub_46D780)
    void note_offline(Client& me, uint32_t to, const std::string& text); // 8561: FriendSMSOfflineCmd
    void note_refused(Client& me, uint32_t from);                     // 8573: our client takes no notes
    void position(Client& me, uint32_t uid);                          // 8551: FriendOfficePosInfoCmd
    void change_zone_to(Client& me, uint32_t uid);                    // 8586: FriendChangeZoneToCmd

    cGameServer& srv_;
    std::recursive_mutex mx_;
    std::unordered_map<uint32_t, Client> clients_;                   // by community session id
};
