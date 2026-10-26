#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include "packet.hpp"

class cGameServer;

// ─────────────────────────────────────────────────────────────────────────────
//  ChatServer — the retail SystemServer's chat module (SystemServer\Chat\ChatServer.cpp; OnCmd sub_437400) on the
//  community connection (system_session.cpp), for what the 2014 client uses of it: the whisper.
//   - Login, C2S 6001 (empty, the client's registration at S2C 5506): sub_436940 -> ChatClient sub_427D80 takes the
//     character's name + uid and enters the lobby -> S2C 6003 [u16 client id][u16 lobby room id] (a failed lobby:
//     6002 [u16 id]). VERIFIED in the client: sub_8457A0 case 6003 keeps the id (+42), sets the chat flag (+40),
//     and the lobby controller (LobbyChatController sub_8493C0) keeps the room id (+24) — the first u16 of its
//     later commands; S2C to a room are routed by that id (sub_8456C0 / sub_845620 -> vtbl+104 sub_849370) and
//     read again from the start.
//   - Whisper, C2S 6007 [u16 room][u8 1][str to][str text] (client sub_849530 = LobbyChatController vtbl+32, the
//     ChatInput's whisper; the line "To <name>" is the client's own): sub_436B00 -> the room's LobbyConnector
//     sub_42DAF0 -> sub_430510: the room's connector of that name (iStrHashGen: case-insensitive) gets S2C 6035
//     [u16 room][u8 1][str from][str text] (client vtbl+260 sub_842E70 -> its chat window), the sender S2C 6009
//     [u16 room][u8 1][str to] (client: a nullsub); no such name: 6008, the same body (client sub_843210 -> the
//     failure line). [u8 0] is a room chat ([u16 room][str text], sub_42DC80): chat rooms are not done.
//   Not done: the whisper log (sub_449660 under dword_7235EC), the chat rooms / notices (6010 .. 6077).
// ─────────────────────────────────────────────────────────────────────────────
class ChatServer
{
public:
    explicit ChatServer(cGameServer& srv) : srv_(srv) {}

    void login(uint32_t sys_sid, uint32_t char_uid, bool paula);   // C2S 6001
    void logout(uint32_t sys_sid);                                  // the community connection closes
    void command(uint32_t sys_sid, PacketBuffer& pkt, uint16_t op, bool paula);   // the other C2S 6002 .. 6077

private:
    void whisper(uint32_t sys_sid, uint16_t room, const std::string& to, const std::string& text, bool paula);
    static std::string key(const std::string& name);                // the case-insensitive lookup key

    struct Client { uint16_t id = 0; uint32_t uid = 0; std::string name; };
    static constexpr uint16_t kLobbyRoom = 1;                       // the one room everyone is in

    cGameServer& srv_;
    std::mutex   mx_;
    std::unordered_map<uint32_t, Client>      clients_;             // by community session id
    std::unordered_map<std::string, uint32_t> names_;               // key(name) -> community session id
    uint16_t     next_id_ = 1;
};
