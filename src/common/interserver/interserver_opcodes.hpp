#pragma once
#include <cstdint>

// Well-known inter-server opcodes (reverse/specs/03_interserver_transport.md).
// One flat u16 opcode space on the Master hub; prefixes below are naming BANDS
// (dispatch is a single table, routed by source peer index — not by family).
namespace paula::inter {

enum : uint16_t {
    // Peer handshake (dialer -> acceptor)
    SVR_LOGIN             = 3000,   // {serverId, name, ports...} length-prefixed stream
    SVR_LOGIN_ACK_CANCEL  = 3001,   // acceptor rejects
    SVR_LOGIN_ACK_OK      = 3002,   // [?] success reply (confirm against binary)

    // Login -> Game client handoff
    LCMD_SVR_LOGIN        = 2035,   // Login announces itself to Game
    CMD_CREATE_CLIENT     = 1,      // client session blob (seq/crc + char record)

    // Zone (Game <-> Master), from spec 06
    MCMD_LEAVE_ZONE       = 3003,
    MCMD_LEAVE_ZONE_ACK_OK      = 3004,
    MCMD_LEAVE_ZONE_ACK_CANCEL  = 3005,
    MCMD_ENTER_ZONE       = 3009,
    MCMD_ENTER_ZONE_ACK_OK      = 3010,
    MCMD_ENTER_ZONE_ACK_CANCEL  = 3011,

    // ProjectPAULA-internal char relay (Login -> Master -> CharStore). Request bodies
    // are prefixed with a u32 reqId for reply correlation; replies echo it back.
    MREQ_CHAR_LIST   = 40001,  // body: u32 owner  -> MACK: u8 usable_slot + u8 count + count×CharRecord
    MACK_CHAR_LIST   = 40002,
    MREQ_CHAR_CREATE = 40003,  // body: CharRecord -> MACK: u32 code (0 = made) [+ CharRecord when 0]
    MACK_CHAR_CREATE = 40004,
    MREQ_CHAR_DELETE = 40005,  // body: u32 owner + u32 charUid + str ip -> MACK: u8 ok
    MACK_CHAR_DELETE = 40006,
};

// Opcode bands on the Master switch (spec 03). Comments only — dispatch is one table.
//   OnICmd*  Item ops      (relayed from Game)
//   OnCCmd*  Char/client   (relayed from Game/Login)
//   OnUCmd*  User/account  (from Login)
//   OnMCmd*  Map/zone      (from Game zones)
//   OnTCmd*  transfer      [?]

} // namespace paula::inter
