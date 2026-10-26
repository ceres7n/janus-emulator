#include "system_session.hpp"
#include "game_server.hpp"
#include "party.hpp"
#include "logger.hpp"

// SystemServer ClientProxy::HandleClientMessage (sub_5620A0) + PartyServer::OnCmd for what it routes.
void SystemClientSession::on_packet(PacketBuffer pkt)
{
    const uint16_t op = static_cast<uint16_t>(pkt.opcode());
    try {
        switch (op) {
        case 0x1581: {                                 // 5505: sub_562100
            const uint64_t uid    = pkt.read<uint64_t>();
            const uint32_t ticket = pkt.read<uint32_t>();
            // Hardening (not retail): the retail SystemServer took the ticket alone; the ticket's character
            // must also be loaded on one of our game connections from this connection's address (release
            // spec), so a ticket seen elsewhere cannot log another machine in as that character.
            if (!server_.check_system_ticket(static_cast<uint32_t>(uid), ticket) ||
                !server_.char_online_from(static_cast<uint32_t>(uid), remote_address())) {
                // (retail: no answer — the client's request times out and it asks again with 5508)
                LOG_INFO("SystemSession %u: USER LOGIN FAILED (2) chr_uid - %llu crc - %u", session_id(),
                         static_cast<unsigned long long>(uid), ticket);
                return;
            }
            uid_ = static_cast<uint32_t>(uid);
            server_.party().attach_system(uid_, session_id());
            PacketBuffer p(Opcode::SESSION_OPEN, is_paula());   // 5506 (the 2014 client reads a u16 list)
            p.write<uint16_t>(0);
            send(p);
            LOG_INFO("SystemSession %u: USER LOGIN SUCCESS chr_uid - %u", session_id(), uid_);
            return;
        }
        case 0x1587:                                   // 5511: the keepalive
            return;
        case 6001:                                     // the ChatServer's login (chat_server.cpp)
            if (uid_) server_.chat().login(session_id(), uid_, is_paula());
            return;
        case 8501:                                     // the FriendServer's login (friend_server.cpp)
            if (uid_) server_.friends().login(session_id(), uid_, is_paula());
            return;
        case 10002: case 11001: case 11102: case 11901:
            return;                                    // the other modules' empty registrations
        default:
            break;
        }
        if (uid_ && ((op >= 7001 && op <= 7088) || (op >= 7301 && op <= 7327))) {
            party_dispatch(server_.party(), uid_, pkt, op);
            return;
        }
        if (uid_ && op >= 6002 && op <= 6077) {
            server_.chat().command(session_id(), pkt, op, is_paula());
            return;
        }
        if (uid_ && op >= 8502 && op <= 8588) {
            server_.friends().command(session_id(), pkt, op, is_paula());
            return;
        }
        if (uid_) LOG_INFO("SystemSession %u: UNHANDLED C2S %u (%zu bytes, chr %u)", session_id(), op, pkt.size(), uid_);
        else      LOG_DEBUG("SystemSession %u: C2S %u before the login - dropped (%zu bytes)", session_id(), op, pkt.size());
    } catch (const std::exception& e) {
        LOG_ERROR("SystemSession %u: C2S %u malformed: %s", session_id(), op, e.what());
    }
}

void SystemClientSession::on_closed()
{
    if (closed_) return;
    closed_ = true;
    if (uid_) server_.party().detach_system(uid_, session_id());
    server_.chat().logout(session_id());
    server_.friends().logout(session_id());
    LOG_INFO("SystemSession %u: closed (chr %u)", session_id(), uid_);
}
