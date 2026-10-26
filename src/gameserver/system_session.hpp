#pragma once
#include <cstdint>
#include "session.hpp"

class cGameServer;

// ─────────────────────────────────────────────────────────────────────────────
//  SystemClientSession — the client's community connection, the retail SystemServer's ClientProxy
//  (SystemServer sub_5620A0). The client keeps two TCP connections: the game one and this one, opened to
//  the address of S2C 0x1585 (5509, [u64 chr uid][str ip][u32 port][u32 ticket]) by c2s_5505 (client
//  sub_82B6E0 -> sub_82A560 connect), which then logs in with C2S 0x1581 (5505) [u64 chr uid][u32 ticket]
//  -> S2C 0x1582 (5506) [u16 0] (client session_open_5506: the community flag byte_11364B8 the party
//  window needs — without it "NotUsePartyDisconnect" — then the module registrations 6001 .. 11901).
//  Every community command (client sub_83D570 -> unk_111FF64) comes this way: the party (7001 .. 7088)
//  and the party board (7302 .. 7325) go to the PartyServer. C2S 0x1587 (5511) is its keepalive (60 s).
//  Not connected, the client asks the game connection again every 10 s (C2S 0x1584, 5508).
// ─────────────────────────────────────────────────────────────────────────────
class SystemClientSession : public Session
{
public:
    SystemClientSession(asio::ip::tcp::socket socket, cGameServer& srv)
        : Session(std::move(socket)), server_(srv) {}

    void on_packet(PacketBuffer pkt);
    void on_closed();
    uint32_t char_uid() const { return uid_; }

private:
    cGameServer& server_;
    uint32_t     uid_{ 0 };      // the character logged in on it (0: none yet)
    bool         closed_{ false };   // on_closed ran (a read and a write can both report the close)
};
