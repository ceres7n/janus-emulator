#include "chat_server.hpp"
#include "game_server.hpp"
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
}  // namespace

std::string ChatServer::key(const std::string& name)
{
    std::string k = name;
    for (char& c : k) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return k;
}

// ChatServer case 6001 (sub_436940): an old ChatClient of this connection goes first (vtbl+16), then a new one
// with the character's name and uid (the ClientProxy +32878 / +32870) into the lobby (sub_427F70) -> S2C 6003.
void ChatServer::login(uint32_t sys_sid, uint32_t char_uid, bool paula)
{
    std::string name;
    {
        const auto rows = srv_.db().query("SELECT name FROM characters WHERE uid=%u", char_uid);
        if (!rows.empty()) name = rows[0].at("name");
    }
    if (name.empty()) {
        LOG_INFO("CHAT SERVER    LOGIN FAIL - chr_uid : %u", char_uid);
        return;
    }
    uint16_t id = 0;
    {
        std::lock_guard<std::mutex> lk(mx_);
        auto it = clients_.find(sys_sid);
        if (it != clients_.end()) {
            auto n = names_.find(key(it->second.name));
            if (n != names_.end() && n->second == sys_sid) names_.erase(n);
            clients_.erase(it);
        }
        id = next_id_++;
        if (next_id_ == 0) next_id_ = 1;
        Client c;
        c.id   = id;
        c.uid  = char_uid;
        c.name = name;
        clients_[sys_sid] = c;
        names_[key(name)] = sys_sid;                         // (a second login of the name takes it over)
    }
    PacketBuffer p(static_cast<Opcode>(6003), paula);
    p.write<uint16_t>(id);
    p.write<uint16_t>(kLobbyRoom);
    srv_.send_to_system(sys_sid, p);
    LOG_INFO("ChatServer: %s (chr %u) in the lobby, client %u", name.c_str(), char_uid, id);
}

void ChatServer::logout(uint32_t sys_sid)
{
    std::lock_guard<std::mutex> lk(mx_);
    auto it = clients_.find(sys_sid);
    if (it == clients_.end()) return;
    auto n = names_.find(key(it->second.name));
    if (n != names_.end() && n->second == sys_sid) names_.erase(n);
    clients_.erase(it);
}

// ChatServer::OnCmd (sub_437400): a command of a connection without a ChatClient is dropped (but 6001).
void ChatServer::command(uint32_t sys_sid, PacketBuffer& pkt, uint16_t op, bool paula)
{
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (clients_.find(sys_sid) == clients_.end()) return;
    }
    switch (op) {
    case 6007: {                                             // sub_436B00
        const uint16_t room = pkt.read<uint16_t>();
        if (pkt.read<uint8_t>()) {
            std::string to   = read_cstr(pkt);
            std::string text = read_cstr(pkt);
            // Hardening (not retail): cut to the client's chat-box limit (kClientChatMax, game_server.hpp) — the
            // line is relayed to another player (the name only comes back to the sender).
            cut_client_text(to, kClientChatMax);
            cut_client_text(text, kClientChatMax);
            whisper(sys_sid, room, to, text, paula);
        } else {
            LOG_DEBUG("ChatServer: room chat (room %u) — chat rooms not done", static_cast<unsigned>(pkt.read<uint16_t>()));
        }
        return;
    }
    default:
        LOG_DEBUG("ChatServer: C2S %u not done (%zu bytes)", op, pkt.size());
        return;
    }
}

// LobbyConnector sub_42DAF0 + sub_430510.
void ChatServer::whisper(uint32_t sys_sid, uint16_t room, const std::string& to, const std::string& text, bool paula)
{
    (void)room;
    std::string from;
    uint32_t target = 0;
    {
        std::lock_guard<std::mutex> lk(mx_);
        auto me = clients_.find(sys_sid);
        if (me == clients_.end()) return;
        from = me->second.name;
        auto n = names_.find(key(to));
        if (n != names_.end() && clients_.count(n->second)) target = n->second;
    }
    if (target) {
        PacketBuffer w(static_cast<Opcode>(6035), paula);
        w.write<uint16_t>(kLobbyRoom);
        w.write<uint8_t>(1);
        write_str(w, from);
        write_str(w, text);
        srv_.send_to_system(target, w);
    }
    PacketBuffer r(static_cast<Opcode>(target ? 6009 : 6008), paula);
    r.write<uint16_t>(kLobbyRoom);
    r.write<uint8_t>(1);
    write_str(r, to);
    srv_.send_to_system(sys_sid, r);
    LOG_DEBUG("ChatServer: whisper %s -> %s%s", from.c_str(), to.c_str(), target ? "" : " (not online)");
}
