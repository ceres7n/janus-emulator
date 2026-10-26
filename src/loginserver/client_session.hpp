#pragma once
#include "session.hpp"
#include "database.hpp"
#include "store/char_record.hpp"
#include <vector>
#include <string>

class cLoginServer;

enum class ClientState { WaitingAuth, CharSelect, InWorld };

// ClientSession — one connected char-select client on the LoginServer.
// (Real LoginServer keeps these in an index-based slot array; we use one object
// per connection. Client-facing opcodes are LCMD_*; send() uses the client cipher.)
class ClientSession : public Session
{
public:
    ClientSession(asio::ip::tcp::socket socket, cLoginServer& srv)
        : Session(std::move(socket)), server_(srv) {}

    void on_packet(std::shared_ptr<Session> self, PacketBuffer pkt);
    void on_close (std::shared_ptr<Session> self);
    void begin_handshake();

private:
    cLoginServer& server_;
    ClientState   state_{ ClientState::WaitingAuth };
    uint32_t      account_id_{ 0 };
    uint8_t       gm_level_{ 0 };

    std::vector<CharRecord> chars_;
    uint8_t       usable_slot_{ 0 };     // the account's character slots (user_info.usable_slot)

    void handle_session_ready     (PacketBuffer& pkt);
    void handle_session_ack       (PacketBuffer& pkt);
    void handle_auth_token        (PacketBuffer& pkt);
    void handle_char_list_request (PacketBuffer& pkt);
    void handle_char_create       (PacketBuffer& pkt);
    void handle_char_delete       (PacketBuffer& pkt);
    void handle_char_select       (PacketBuffer& pkt);
    void handle_slot_query        (PacketBuffer& pkt);
    void handle_heartbeat         (PacketBuffer& pkt, Opcode op);

    void send_session_open();
    void send_auth_ack();
    void send_world_ack();
    void send_create_fail(uint32_t code);
    void send_char_entry(PacketBuffer& pkt, const CharRecord& c);
    void send_map_server_info(const CharRecord& chr);
};
