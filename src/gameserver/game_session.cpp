#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include "filetime.hpp"
#include <random>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <utility>
#include <algorithm>
#include <cmath>

// The first `max` of `n` bytes as "AB CD ..", for a LOG_DEBUG argument only: once LOG_DEBUG checks the level
// before it evaluates its arguments (logging policy), the dump is built only when debug lines are kept.
static std::string hex_bytes(const uint8_t* p, size_t n, size_t max)
{
    std::string s; char b[4];
    for (size_t i = 0; i < n && i < max; ++i) { snprintf(b, sizeof(b), "%02X ", p[i]); s += b; }
    return s;
}

void GameClientSession::begin_handshake()
{
    if (is_paula()) {
        LOG_DEBUG("GameClientSession %u: Paula mode active, awaiting 0x0004 auth", session_id());
        return;
    }
    // Fallback: old non-Paula handshake (not used with this client build)
    std::random_device rd;
    uint32_t seed = rd();
    set_crypt_key(seed);
    PacketBuffer pkt(Opcode::SESSION_ENCRYPT);
    pkt.write<uint32_t>(seed);
    send(pkt);
}

void GameClientSession::on_packet(std::shared_ptr<Session> /*self*/, PacketBuffer pkt)
{
    Opcode op = pkt.opcode();
    // Trace EVERY incoming C2S opcode (diagnosing movement/control: we need to see
    // whether the client even emits C2S 0x0018 MOVE when the user clicks to walk).
    LOG_DEBUG("GameClientSession %u: <<< C2S 0x%04X (state=%d, %zu bytes)",
              session_id(), static_cast<uint16_t>(op), static_cast<int>(state_), pkt.size());
    // Hardening (not retail): a closed session runs nothing more — after a double login closed it (kick_char)
    // the packets it had already read must not act on the character the new session holds now.
    if (state_ == GameClientSessionState::Closed) return;
    // Hardening (not retail): after S2C 154 this is a field change's old connection — the client drops it and comes
    // back with C2S 156 on a new one (game_live.log: no C2S ever followed a 154 on its connection). What a modified
    // client still sent here would act on a stale copy (money_, the bag) of the character the 156 session holds:
    // a rollback dupe (GSC-04). Only the heartbeats are answered.
    if (relocated_ && op != Opcode::HEARTBEAT_A && op != Opcode::HEARTBEAT_B) {
        LOG_DEBUG("GameClientSession %u: C2S 0x%04X after the 154 - dropped", session_id(), static_cast<uint16_t>(op));
        return;
    }
    // Hardening (not retail): until a C2S 0x0004 / 156 has taken its ticket and loaded a character, only those,
    // the community address requests and the heartbeats are handled. The retail GameServer had no player
    // object before the enter to run anything else on; our handlers would run on an empty session (uid 0).
    // (Handshake = no enter completed: a 0x0004 sets Loading at its end, a 156 InWorld.)
    if (state_ == GameClientSessionState::Handshake) {
        switch (op) {
        case Opcode::C2S_MAP_AUTH:      case Opcode::C2S_ENTER_ZONE_CMD:
        case Opcode::C2S_SESSION_READY: case Opcode::C2S_SESSION_ACK:
        case Opcode::HEARTBEAT_A:       case Opcode::HEARTBEAT_B:
            break;
        default:
            LOG_DEBUG("GameClientSession %u: C2S 0x%04X before the enter - dropped", session_id(),
                      static_cast<uint16_t>(op));
            return;
        }
    }
    // Hardening (not retail): the 0x039F second-password wrapper hands its inner command back to us
    // (handle_secure_command, inventory.cpp). A packet may come back once, never deeper: a chain of wrappers
    // recursed until the stack overflowed — an SEH fault no catch here stops, so the process died.
    if (dispatch_depth_ >= 2) {
        LOG_INFO("GameClientSession %u: C2S 0x%04X nested twice in 0x039F - dropped", session_id(),
                 static_cast<uint16_t>(op));
        return;
    }
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& v) : d(v) { ++d; }
        ~DepthGuard() { --d; }
    } depth_guard(dispatch_depth_);
    if (trade_ && trade_blocks(pkt, static_cast<uint16_t>(op))) return;   // the trade's state 6 (trade.cpp)

    switch (op) {
    // The player-to-player trade (trade.cpp): 94 ask, 97 yes, 99 no, 100 cancel, 101 the offer, 102 checked,
    // 171 / 172 / 173 / 176 / 177 the final step, 179 the window's chat.
    case static_cast<Opcode>(0x005E): case static_cast<Opcode>(0x0061): case static_cast<Opcode>(0x0063):
    case static_cast<Opcode>(0x0064): case static_cast<Opcode>(0x0065): case static_cast<Opcode>(0x0066):
    case static_cast<Opcode>(0x00AB): case static_cast<Opcode>(0x00AC): case static_cast<Opcode>(0x00AD):
    case static_cast<Opcode>(0x00B0): case static_cast<Opcode>(0x00B1): case static_cast<Opcode>(0x00B3):
        handle_trade(pkt, static_cast<uint16_t>(op));
        break;
    // The post box (postbox.cpp): 1230 a list, 1233 a detail, 1236 take, 1239 send, 1242 delete (the last three
    // come inside the 0x039F second-password wrapper, which re-posts them here).
    case static_cast<Opcode>(1230): handle_post_list(pkt);   break;
    case static_cast<Opcode>(1233): handle_post_detail(pkt); break;
    case static_cast<Opcode>(1236): handle_post_recv(pkt);   break;
    case static_cast<Opcode>(1239): handle_post_send(pkt);   break;
    case static_cast<Opcode>(1242): handle_post_delete(pkt); break;
    case Opcode::C2S_MAP_AUTH:              handle_auth(pkt);            break;
    case Opcode::MAP_ENTER_ACK:             handle_enter_confirm(pkt);   break;  // C2S 0x0005: client is in-world & ready
    case Opcode::C2S_MOVE:                  handle_move(pkt);            break;
    case static_cast<Opcode>(0x01D4):       handle_portal_move(pkt);     break;  // walk into a portal
    case Opcode::C2S_GUIDE_REQ: {  // client requested guide — reply with empty 0x04A6 so it stops waiting
        PacketBuffer g(Opcode::GAME_UNK_4A6, is_paula()); g.write<uint16_t>(0); send(g);
        LOG_DEBUG("GameClientSession %u: C2S 0x04A5 guide req -> sent empty 0x04A6", session_id());
        break; }
    case Opcode::C2S_SESSION_READY:         handle_session_ready(pkt);   break;
    case Opcode::C2S_SESSION_ACK:           handle_session_ack(pkt);     break;
    case Opcode::HEARTBEAT_A:
    case Opcode::HEARTBEAT_B:               handle_heartbeat(pkt, op);   break;
    case Opcode::C2S_ENTER_ZONE_CMD:        handle_enter_zone_cmd(pkt);  break;  // 156: seamless field change
    // The janus-era field commands (0x2CD4 / 0x2CD7 / 0x2C89 / 0x2C8C / 0x2C95 / 0x2C98) and 0x271C: the KR
    // client never sends them (0 of 6,241 C2S in game_live.log; a field change is S2C 154 -> C2S 156). Their
    // handlers moved / spawned / despawned the player with no check and are gone; ignored here on purpose
    // (not logged as unhandled).
    case Opcode::C2S_ZONE_LOAD:   case Opcode::C2S_ZONE_ENTER:  case Opcode::C2S_FIELD_ENTER:
    case Opcode::C2S_FIELD_READY: case Opcode::C2S_FIELD_LEAVE: case Opcode::C2S_FIELD_POSITION_UPD:
    case Opcode::C2S_DRILL_DISPATCH:
        break;
    // The party (party.cpp): 7001 make, 7013 ask, 7017 yes, 7020 no, 7021 leave, 7025 kick, 7010 master,
    // 7042 chat, 7004 division, 7007 name, 7054 item mode, 7079 requests welcome, 7047 drop a dull one,
    // 7066 / 7077 / 7080 join requests, 7069 / 7072 their answers, 7075 dissolve, 7083 take the invitation
    // back; and the empty registrations with the community modules (S2C 5506 -> client sub_83D920).
    case static_cast<Opcode>(0x1B59): case static_cast<Opcode>(0x1B65): case static_cast<Opcode>(0x1B69):
    case static_cast<Opcode>(0x1B6C): case static_cast<Opcode>(0x1B6D): case static_cast<Opcode>(0x1B71):
    case static_cast<Opcode>(0x1B62): case static_cast<Opcode>(0x1B82): case static_cast<Opcode>(0x1B5C):
    case static_cast<Opcode>(0x1B5F): case static_cast<Opcode>(0x1B8E): case static_cast<Opcode>(0x1BA7):
    case static_cast<Opcode>(0x1B87): case static_cast<Opcode>(0x1B9A): case static_cast<Opcode>(0x1BA5):
    case static_cast<Opcode>(0x1BA8): case static_cast<Opcode>(0x1B9D): case static_cast<Opcode>(0x1BA0):
    case static_cast<Opcode>(0x1BA3): case static_cast<Opcode>(0x1BAB):
    // the party board: 7302 / 7305 lists, 7308 / 7311 posts, 7314 / 7317 deletes, 7320 / 7321 searches,
    // 7322 / 7325 details
    case static_cast<Opcode>(0x1C86): case static_cast<Opcode>(0x1C89): case static_cast<Opcode>(0x1C8C):
    case static_cast<Opcode>(0x1C8F): case static_cast<Opcode>(0x1C92): case static_cast<Opcode>(0x1C95):
    case static_cast<Opcode>(0x1C98): case static_cast<Opcode>(0x1C99): case static_cast<Opcode>(0x1C9A):
    case static_cast<Opcode>(0x1C9D):
        handle_party(pkt, static_cast<uint16_t>(op)); break;
    case static_cast<Opcode>(0x1771): case static_cast<Opcode>(0x2135): case static_cast<Opcode>(0x2712):
    case static_cast<Opcode>(0x2AF9): case static_cast<Opcode>(0x2B5E): case static_cast<Opcode>(0x2E7D):
        break;                                                                   // 6001 8501 10002 11001 11102 11901
    case Opcode::C2S_CHAT:                                                       // 54: the normal chat
    case static_cast<Opcode>(0x01A1):                                            // 417: a type-23 zone's
    case static_cast<Opcode>(0x02C9):       handle_chat(pkt, op);        break;  // 713: an observer's
    case static_cast<Opcode>(0x0320):       handle_megaphone(pkt);       break;  // 800: the Mic (megaphone.cpp)
    // The Memory Port (memory_port.cpp): its window opens (817), a point saved (820) or dropped (823).
    case static_cast<Opcode>(0x0331):       handle_memory_port_open(pkt);   break;
    case static_cast<Opcode>(0x0334):       handle_memory_port_add(pkt);    break;
    case static_cast<Opcode>(0x0337):       handle_memory_port_delete(pkt); break;
    // Skill use (retail Player state machine cases 238 / 224 / 263, see skill.cpp).
    case static_cast<Opcode>(0x00EE):       handle_skill_request(pkt);   break;  // use request
    case static_cast<Opcode>(0x00E0):       handle_skill_target(pkt);    break;  // selected target
    case static_cast<Opcode>(0x0107):       handle_skill_select_cancel(pkt); break;  // selection cancelled
    case static_cast<Opcode>(0x034C):       handle_skill_scroll(pkt);    break;  // a skill scroll (case 844)
    case static_cast<Opcode>(0x0108):       handle_use_item_custom(pkt); break;  // use with custom data (case 264)
    case static_cast<Opcode>(0x0401):       handle_disguise_cancel(pkt); break;  // end the disguise (case 1025)
    case static_cast<Opcode>(0x0351):       handle_skill_scroll_target(pkt); break;  // its target (case 849)
    case static_cast<Opcode>(0x01E1):       handle_skill_level_up(pkt);  break;  // raise a skill's level
    // NPC talk (npc_talk.cpp). C2S 0x0075 (client sub_6DDB70: the NPC dialog closes, also on an
    // attack): Player case 117 only exists in the NPC-talk state 9 (-> state 0).
    case static_cast<Opcode>(0x006F):       handle_npc_talk(pkt);        break;
    case static_cast<Opcode>(0x0072):       handle_npc_next(pkt);        break;
    case static_cast<Opcode>(0x0075):       handle_npc_end(pkt);         break;
    case static_cast<Opcode>(0x007F):       handle_npc_script(pkt);      break;
    // Basic melee (captured live): 0x001D = engage target [u16 eid][u8], 0x0120 = swing [u16 eid].
    case static_cast<Opcode>(0x001D):
    case static_cast<Opcode>(0x0120):       handle_attack(pkt, op);      break;
    case static_cast<Opcode>(0x0021):       handle_pickup(pkt);          break;  // pick up a ground item
    case static_cast<Opcode>(0x039F):       handle_secure_command(pkt);  break;  // 2nd-password wrapper (drop...)
    case static_cast<Opcode>(0x0043):       handle_drop_item(pkt);       break;  // CMD_DROP_ITEM (inside 0x039F)
    case static_cast<Opcode>(0x0057):       handle_use_item(pkt);        break;  // use an item
    case static_cast<Opcode>(0x0051):       handle_equip_item(pkt);      break;  // equip
    case static_cast<Opcode>(0x0054):       handle_release_item(pkt);    break;  // take off
    case static_cast<Opcode>(0x0205):       handle_equip_common(pkt);    break;  // equip a common item (bullets)
    case static_cast<Opcode>(0x0208):       handle_release_common(pkt);  break;  // take a common item off
    case static_cast<Opcode>(0x01FF):       handle_gun_seed();           break;  // a gun's CRC seed
    case static_cast<Opcode>(0x01DD):       handle_gun_attack(pkt);      break;  // CMD_BEGIN_ATTACK_GUN
    case static_cast<Opcode>(0x0016):       // death screen "Go to Town" (restart in a town)
    case static_cast<Opcode>(0x02F7):       handle_restart(op);          break;  // "Restart" (in place)
    case static_cast<Opcode>(0x006C):       handle_stat_alloc(pkt);      break;  // stat distribution "accept"
    case static_cast<Opcode>(0x0040):       handle_action(pkt);          break;  // sit / rest / stand
    // NPC shops (shop.cpp): open (a kind-5 object clicked), the buy list / sell rates, buy, sell
    // (inner 76 of 0x039F), close.
    case static_cast<Opcode>(0x0046):       handle_shop_open(pkt);       break;
    case static_cast<Opcode>(0x00A4):       handle_shop_list(pkt, false); break;
    case static_cast<Opcode>(0x00A6):       handle_shop_list(pkt, true); break;
    case static_cast<Opcode>(0x0048):       handle_shop_buy(pkt);        break;
    case static_cast<Opcode>(0x004C):       handle_shop_sell(pkt);       break;
    case static_cast<Opcode>(0x004F):       handle_shop_close();         break;
    // Drilling (drill.cpp): start at / walk to a point, cancel, the client's result (CMD_END_DRILL).
    case static_cast<Opcode>(0x002C):       handle_drill_start(pkt);     break;
    case static_cast<Opcode>(0x0030):       handle_drill_cancel(pkt);    break;
    case static_cast<Opcode>(0x0031):       handle_drill_end(pkt);       break;
    // Exchange shops (exchange_shop.cpp): an exchange's details, the exchange, the window closes.
    case static_cast<Opcode>(0x00FB):       handle_exchange_info(pkt);   break;
    case static_cast<Opcode>(0x00FE):       handle_exchange(pkt);        break;
    case static_cast<Opcode>(0x0101):       handle_exchange_close();     break;
    // The monster quest's return portal (monster_quest.cpp).
    case static_cast<Opcode>(0x0410):       handle_monq_return(pkt);     break;
    // The warehouse (warehouse.cpp): bag -> tab, tab -> bag, tab -> tab, the window closes.
    case static_cast<Opcode>(0x008B):       handle_wh_add(pkt);          break;
    case static_cast<Opcode>(0x008E):       handle_wh_remove(pkt);       break;
    case static_cast<Opcode>(0x02E9):       handle_wh_exchange(pkt);     break;
    case static_cast<Opcode>(0x008A):       handle_wh_close();           break;
    // The NeoWarehouse, the account's bank (bank.cpp): bag -> bank, bank -> bag, gelder in / out, close.
    case static_cast<Opcode>(0x00C6):       handle_bank_add(pkt);        break;
    case static_cast<Opcode>(0x00C9):       handle_bank_remove(pkt);     break;
    case static_cast<Opcode>(0x00CC):       handle_bank_money(pkt, true);  break;
    case static_cast<Opcode>(0x00CF):       handle_bank_money(pkt, false); break;
    case static_cast<Opcode>(0x00C5):       handle_bank_close();         break;
    // Item refine / mature compound / reset (refine.cpp, states 30 / 31 / 32): the requests and
    // the window-closed commands.
    case static_cast<Opcode>(0x02B8):       handle_refine(pkt);          break;  // 696
    case static_cast<Opcode>(0x02BB):       handle_repair(pkt);          break;  // 699
    case static_cast<Opcode>(0x02C0):       handle_growth(pkt);          break;  // 704
    case static_cast<Opcode>(0x02DE):       handle_resetmix(pkt);        break;  // 734
    case static_cast<Opcode>(0x02BE):                                            // 702
    case static_cast<Opcode>(0x02C3):                                            // 707
    case static_cast<Opcode>(0x02E1):       handle_refine_close(static_cast<uint16_t>(op)); break;  // 737
    // Compounds (compound.cpp): a compound's details, the roll, the compound, open / close the window.
    case static_cast<Opcode>(0x00DD):       handle_compound_info(pkt);   break;
    case static_cast<Opcode>(0x00E3):       handle_compound_prob(pkt);   break;
    case static_cast<Opcode>(0x00BA):       handle_compound(pkt);        break;
    case static_cast<Opcode>(0x00BB):       handle_compound_open();      break;
    case static_cast<Opcode>(0x00C0):       handle_compound_close();     break;
    // Skill / job masters (skill_shop.cpp): open, the next message, the scripts, the shop lists, close,
    // mastery; the skill toggles are refused while talking to one (state 25: S2C 276 / 279).
    case static_cast<Opcode>(0x01E6):       handle_skill_shop_open(pkt); break;
    case static_cast<Opcode>(0x01E9):       handle_skill_shop_next(pkt); break;
    case static_cast<Opcode>(0x01F0):       handle_skill_shop_script(pkt, false); break;
    case static_cast<Opcode>(0x01F1):       handle_skill_shop_script(pkt, true);  break;
    case static_cast<Opcode>(0x01F7):       handle_skill_shop_list(pkt, false);   break;
    case static_cast<Opcode>(0x01F9):       handle_skill_shop_list(pkt, true);    break;
    case static_cast<Opcode>(0x01EC):
    case static_cast<Opcode>(0x01F6):       handle_skill_shop_close();   break;
    case static_cast<Opcode>(0x0230):       handle_skill_master(pkt);    break;
    case static_cast<Opcode>(0x0239):       handle_change_job(pkt);      break;
    case static_cast<Opcode>(0x0112):
    case static_cast<Opcode>(0x0115):
        if (sshop_) {                                   // __fputchar_0 / __fputchar_1
            PacketBuffer p(static_cast<Opcode>(static_cast<uint16_t>(op) == 0x0112 ? 0x0114 : 0x0117), is_paula());
            send(p);
        }
        break;
    default: {
        // An unknown C2S (a client feature not done here). INFO names it once per opcode and session (at most 16
        // opcodes: a client sending them at line rate cannot flood the log, GSC-12); the byte-exact capture of the
        // body (its first 48 bytes) is a DEBUG line.
        const uint16_t uop = static_cast<uint16_t>(op);
        const size_t avail = (pkt.size() > pkt.rpos()) ? (pkt.size() - pkt.rpos()) : 0;
        if (unhandled_seen_.size() < 16 &&
            std::find(unhandled_seen_.begin(), unhandled_seen_.end(), uop) == unhandled_seen_.end()) {
            unhandled_seen_.push_back(uop);
            LOG_INFO("GameClientSession %u: UNHANDLED C2S 0x%04X (%zu body bytes)", session_id(), uop, avail);
        }
        LOG_DEBUG("GameClientSession %u: UNHANDLED C2S 0x%04X (%zu body bytes): %s", session_id(), uop, avail,
                  hex_bytes(pkt.data() + pkt.rpos(), avail, 48).c_str());
        break; }
    }
}

// The connection closed (cGameServer::on_session_close, on our strand; once — a read and a write can both
// fail). A logout / drop in the world is the Shells' LogoutPlayer (sub_7048A0 / sub_7D2A10 / sub_7FDF40):
// our progress saved, our states end, the area sees us leave (S2C 6), then we leave the zone. The old
// connection of a field change (the 154 sent) does none of that: leave_field / begin_field_change told the
// area (0x1B + 21), carried our states and saved the destination, and the reconnected session owns the char.
void GameClientSession::on_close(std::shared_ptr<Session> /*self*/)
{
    if (state_ == GameClientSessionState::Closed) return;
    const bool moved  = leaving_ || relocated_;
    const bool logout = !moved && state_ == GameClientSessionState::InWorld;   // (not loaded yet: nothing to save)
    LOG_INFO("GameClientSession %u: disconnecting (char=%u, state=%d%s)...",
             session_id(), char_uid_, static_cast<int>(state_), moved ? ", field change" : "");
    // Robust teardown: a throw here previously could take the whole map-server down on a
    // client disconnect, cascading into "servers down / DC on open" for the next attempt.
    try { trade_close(); } catch (...) {}                   // a trade in its DB step settles first (trade.cpp)
    if (logout) {
        try { save_position(); } catch (const std::exception& e) { LOG_ERROR("on_close save_position: %s", e.what()); } catch (...) {}
    }
    // On every close (ES-02): our effect states give their ids back (es_end_all sends nothing) and those of our
    // skills on others end. A field change's old connection has none left (begin_field_change ended them after
    // es_carry_out copied what goes along); an id kept here was lost for good.
    try { es_end_all(); } catch (...) {}
    try { cast_es_end_all(); } catch (...) {}
    try { end_all_mob_attacks(true); } catch (...) {}       // the monsters fighting us lose their target
    if (logout) {
        try { pet_destroy(false); } catch (...) {}          // our pet goes with us (pet.cpp)
        try {                                               // the Crazy Hammer objects give their ids back (a
            if (crazy_.do_id)    server_.entity_mgr().free_oid(crazy_.do_id);      //   zone change's carry took
            if (crazy_.gauge_id) server_.entity_mgr().free_oid(crazy_.gauge_id);   //   them already)
            crazy_ = CrazyHammer{};
        } catch (...) {}
    }
    try { if (trade_ || trade_opposite_ || trade_id_ >= 0) trade_reset(); } catch (...) {}   // a trade ends (trade.cpp)
    // The area sees us leave (S2C 6, sub_5F76A0). Before the despawn, while we still have the id.
    try {
        if (logout && object_id_)
            server_.player_left(zone_id_, static_cast<uint16_t>(object_id_), session_id());
    } catch (...) {}
    // Only despawn if WE still own the entity. On a seamless warp the client reconnects and the new
    // session takes over this char's stable oid before this (old) connection finishes closing;
    // despawning unconditionally here would delete the entity the new session is now using.
    try { if (object_id_) server_.entity_mgr().despawn_player_if_owner(object_id_, session_id()); } catch (...) {}
    object_id_ = 0;
    state_ = GameClientSessionState::Closed;
    LOG_INFO("GameClientSession %u: disconnected cleanly", session_id());
}

// ─── Handshake ───────────────────────────────────────────────────────────────

void GameClientSession::handle_session_ready(PacketBuffer& /*pkt*/)
{
    send_session_open();
}

// C2S 0x1584 (5508, empty): the client has no community connection and asks where it is (client c2s_5508,
// every 10 s while the flag is down, sub_83D860) — the address again, with a new ticket.
void GameClientSession::handle_session_ack(PacketBuffer& /*pkt*/)
{
    send_session_param_1();
}

void GameClientSession::handle_auth(PacketBuffer& pkt)
{
    // C2S 0x0004: first packet after TCP connect, sent by sub_ACB660 in the client.
    // Layout (14 bytes):
    //   [0..3]  char_uid_lo  u32  — from NLT slot (stored by 0x07D2)
    //   [4..7]  char_uid_hi  u32  — always 0 (uid is u32, hi=0)
    //   [8..11] unknown      u32  — 0 in current captures
    //   [12..13] zone_id     u16  — from sub_9E5670 result (offset[4..5] of 0x07DE payload)
    try {
        const uint32_t uid = pkt.read<uint32_t>();
        uint32_t uid_hi  = pkt.read<uint32_t>(); (void)uid_hi;
        // sub_ACB660 layout: uid_lo(u32) + uid_hi(u32) + zone_id(u16) + unk(u32) = 14 bytes
        // zone_id is at bytes [8..9], NOT [12..13]
        int16_t  zone    = static_cast<int16_t>(pkt.read<uint16_t>());
        uint32_t unk     = pkt.read<uint32_t>(); (void)unk;

        LOG_DEBUG("GameClientSession %u: 0x0004 auth char_uid=%u zone=%d", session_id(), uid, zone);

        // Hardening (not retail): one character per connection — a second 0x0004 / 156 closes it.
        if (char_uid_) {
            LOG_WARN("GameClientSession %u: a second enter (C2S 0x0004, char %u) on the connection of char %u - "
                     "closed", session_id(), uid, char_uid_);
            close();
            return;
        }
        // The Login -> Game ticket (release spec): the LoginServer's row for this character, this address and
        // its owner's account, used once. Without one nothing is sent and the connection closes.
        uint32_t ticket_account = 0;
        if (!take_game_ticket(uid, ticket_account)) {
            close();
            return;
        }
        char_uid_ = uid;
        server_.kick_char(char_uid_, session_id(), peer_ip_, /*community*/true);   // a double login (GSC-04)

        // Session-level params first (the client sends C2S_SESSION_READY (0x1581) only
        // after receiving SESSION_PARAM_1 (0x1585)). The map-enter ACK (0x0005) is NOT
        // sent here anymore — the real server sends it LAST, after the full char/field
        // data batch (see send_world_spawn). Sending it first made the client enter the
        // field before the zone/char data arrived (zone id stayed 0 → NULL-field crash).
        send_session_param_1();
        send_session_param_2();

        if (!load_char_from_db()) {
            char_uid_ = 0;
            return;
        }
        if (account_id_ != ticket_account) {                // (checked with the ticket; changed since?)
            LOG_WARN("GameClientSession %u: char %u belongs to account %u, not to the ticket's %u - closed",
                     session_id(), char_uid_, account_id_, ticket_account);
            char_uid_ = 0;
            close();
            return;
        }
        // Zone::PrepareToStartPlayer (sub_810550 -> sub_810940): the saved point is kept only where a player
        // may start; else the map's default arrival (a new character is saved at (0,0), usp_insert_character).
        {
            const MapGrid* g = server_.zone_grid(zone_id_);
            const bool ok = g ? g->start_point_ok(pos_x_, pos_y_) : (pos_x_ > 0 && pos_y_ > 0);
            if (!ok) {
                int32_t ax = 0, ay = 0;
                server_.maps_db().default_arrival(zone_id_, ax, ay);
                LOG_INFO("GameClientSession %u: Zone::PrepareToStartPlayer - (%d,%d) -> (%d,%d) zone %d", session_id(),
                         pos_x_, pos_y_, ax, ay, zone_id_);
                pos_x_ = ax;
                pos_y_ = ay;
            }
        }
        update_return_town(/*at_login*/true);
        cdm_restore_carry();
        monq_restore_carry();
        pq_restore_carry();
        {
            // (a full login builds a new avatar without the old ESs' buff stats: carried ESs are dropped
            // here rather than made again unseen — only the 156 re-enter keeps the avatar)
            std::vector<cGameServer::EsCarry> drop;
            server_.es_take(char_uid_, drop);
            server_.disguise_take(char_uid_);              // (and a disguise: it is not saved)
            cGameServer::CrazyCarry cc;                    // (and the Crazy Hammer state: a new gauge below)
            if (server_.crazy_take(char_uid_, cc)) {
                if (cc.do_id)    server_.entity_mgr().free_oid(cc.do_id);
                if (cc.gauge_id) server_.entity_mgr().free_oid(cc.gauge_id);
            }
        }

        // (No S2C 0x1B5A here any more. The janus code sent it as "CHAR_INFO" [u16 type][name][u16 hair], but
        // 7002 is the client's party-formed packet: dispatch_packet_main -> dispatch_char_7000 (sub_89F450) case
        // 7002 -> sub_89D650 sets the party id / name / master and adds the local avatar as a member. Sent here,
        // before 0x0005, it reached the char-select task's handler sub_7457A0, which drops 7002 (past its child
        // handler at +384, not identified, it forwards only 5504 / 5509 / 5510 to dispatch_packet_main), and the
        // event pump sub_82D300 -> sub_82C180 ignores the "not handled": the client never used it (a login with it
        // never showed the solo-party HUD the 156 re-entry showed, reverse/specs/09). Its "IDA proof" sub_B3F450 /
        // sub_B3D650 are UI code.)

        // KR self-spawn: create the player entity and push PLAYER_SPAWN_LOGIN (0x0007)
        // + ENTITY_ID_ECHO (0x03A5) so the client builds a valid Player entity during
        // in-world UI init. Without the spawn the client derefs a NULL avatar and
        // crashes at VA 0x33441F while constructing the game UI.
        enter_zone(zone_id_, pos_x_, pos_y_);   // assigns object_id_ (runtime entityId)
        send_world_spawn();                      // full KR Phase 1 + Phase 2 spawn sequence
        server_.party().enter(char_uid_, session_id(), party_info(), is_paula());   // PartyMember (party.cpp)

        state_ = GameClientSessionState::Loading;
    } catch (const std::exception& e) {
        LOG_ERROR("GameClientSession %u: auth error: %s", session_id(), e.what());
        close();
    }
}

void GameClientSession::handle_enter_confirm(PacketBuffer& /*pkt*/)
{
    // C2S 0x0005: the client sends this back AFTER it has entered the field and is ready
    // in-world (log showed it ~9s post-enter, unhandled → silence → client timeout/disconnect).
    // Now the client is running the IN-WORLD dispatcher (sub_6F0960), which DOES handle the
    // self-spawn 0x0007 + ENTITY_ID_ECHO 0x03A5 — so this is the correct moment to spawn the
    // player for real (appearance/position), unlike the pre-0x0005 batch.
    if (state_ == GameClientSessionState::InWorld) return;   // only once
    state_ = GameClientSessionState::InWorld;
    ensure_world_tick();
    LOG_INFO("GameClientSession %u: client confirmed in-world (C2S 0x0005) — in-world, NOT re-spawning self",
             session_id());
    // EXPERIMENT (2026-10-08): do NOT send 0x0007/0x03A5 here.
    // The enter BATCH (0x2A7+0x00E6+0x00E9+0x0005) already made the client build its own
    // local-player avatar as entity id 1 (client sub_6E9170 -> sub_6E1340, which registers
    // the avatar from 0x00E6's entityId and reads appearance/stats from the batch). Our
    // post-enter 0x0007 (PLAYER_SPAWN_LOGIN, same eid=1) then lands on that EXISTING entity
    // and clobbers it: it sets ClassChainId = species (9 -> "Archeologist", not the real job)
    // and carries NO HP (-> HUD shows 0/120). That corrupted/duplicated self is why the
    // avatar renders but is uncontrollable (click produces no C2S 0x0018) and shows wrong
    // class + 0 HP. 0x0007/0x03A5 are for OTHER players, not the local one. So we send
    // nothing self-related here and let the batch be the single source of truth.
    // If the avatar fails to render at all without this, revert — but then the fix is to put
    // correct class/HP into the BATCH (0x00E9 stats region), not to re-send 0x0007.
    // send_player_spawn();   // 0x0007 — DISABLED (corrupts the batch-built local player)
    // send_entity_id_echo(); // 0x03A5 — DISABLED (self-bind already done by the batch)
    // send_entity_list();    // nearby entities — none on a solo map anyway

    // EXPERIMENT (2026-10-08, client RE): ENABLE PLAYER CONTROL.
    // Movement is gated client-side by *(GameWorld+1388) (input-enable flag). The client's
    // GameInTask message handler (sub_6B4E00) has `case 0x135: *(GameWorld+1388)=1;` — i.e.
    // in-world opcode/message 0x135 explicitly ENABLES control. We never send it, so the flag
    // stays 0 and every click is dropped (no C2S 0x0018, no local walk animation). 0x135 (309)
    // is in the in-world opcode range the client dispatches. Sending it empty, right after the
    // client confirms it is in-world, should flip the control flag → Paula walks.
    { PacketBuffer ctrl(static_cast<Opcode>(0x0135), is_paula()); send(ctrl);
      LOG_DEBUG("GameClientSession %u: sent 0x0135 (ENABLE_CONTROL experiment)", session_id()); }

    // Entering the zone at login (LoginAck -> sub_7FD820) protects the player for 3000 ms
    // (sub_73CF80(3000, 0)).
    protect(3000);

    // Now the in-world dispatcher is live and can render other actors: exchange appear packets
    // with everyone already in this zone (they see us, we see them). From here, movement (0x18)
    // and chat (0x39) broadcasts land on existing entities.
    server_.player_entered(*this);

    // LoginAck (sub_7FD820), after the sector enter: sub_5F7040 = our appear as op 7 to the area. VERIFIED
    // in the client: case 7 skips its own avatar's id and runs sub_6F02B0(args, 1) for anyone else — the
    // appear plus the "dung jang" / "etc_warp" effect (the warp-in the others see). Not sent to ourselves:
    // our client drops its own op 7 anyway (the retail sends it to the whole area).
    {
        PacketBuffer ap = build_appear(0x0007);
        server_.broadcast_zone(zone_id_, ap, session_id());
    }
    // Then the equipment's OnLoad (sub_75DEA0): an equipped pet item makes its pet, op 8 to the area
    // (ourselves included — we are in the sector now).
    pet_load(/*to_self*/true);
    crazy_init();                       // the Bear's Crazy Hammer gauge when 4801 is learned (skill_bear.cpp)
    post_login();                       // MCmd 3156: the post box counts, S2C 1228 (postbox.cpp)
}

void GameClientSession::handle_move(PacketBuffer& pkt)
{
    // C2S 0x18 MOVE (Player case 24): [u16 srcX][u16 srcY][u16 dstX][u16 dstY][u8 moveType].
    // The client walks locally regardless; the server updates the entity and broadcasts the
    // move to the zone. Not handling this at all is what caused the walk-time disconnect.
    try {
        if (dead_ || leaving_) return;                // a dead (or leaving) player doesn't move
        if (rest_state_) return;                      // sitting / resting (states 8 / 15) swallow it
        if (shop_) return;                            // so does the shop state 5
        if (exchange_) return;                        // and the exchange shop state 18
        if (compound_) return;                        // and the compound state 13
        if (refine_ui_) return;                       // and the refine / growth / reset states 30..32
        if (sshop_) return;                           // and the skill master state 25
        if (pcond_test(32)) return;                   // case 24: the condition 32 (e.g. Berserker) — ignored
        head_notice_end();                            // the conditions 2 / 3 / 8 end (megaphone.cpp)
        uint16_t srcX = pkt.read<uint16_t>();
        uint16_t srcY = pkt.read<uint16_t>();
        uint16_t dstX = pkt.read<uint16_t>();
        uint16_t dstY = pkt.read<uint16_t>();
        uint8_t  mt   = pkt.read<uint8_t>();
        portal_armed_ = false;                        // case 24: +2968 = 0
        walk(srcX, srcY, dstX, dstY, mt);
    } catch (...) {}
}

// C2S 0x1D4 (Player case 468; client c2s_468, sent by OnGroundClick_Main case 7 when the clicked
// point lies in one of the map's portals): [u16 dstX][u16 dstY][u8 moveType]. The same walk as a
// 0x18 but from where the server has us (vtbl+0xD4 from the current position, then S2C 0x18
// sub_5F5240 [id][cur][dest][type]), and it arms the portal: +2968 = 1, +2969 = the destination.
void GameClientSession::handle_portal_move(PacketBuffer& pkt)
{
    try {
        if (dead_ || leaving_) return;
        if (pcond_test(32)) return;                   // case 468: the condition 32 — ignored
        head_notice_end();                            // the conditions 2 / 3 / 8 end (megaphone.cpp)
        const uint16_t dstX = pkt.read<uint16_t>();
        const uint16_t dstY = pkt.read<uint16_t>();
        const uint8_t  mt   = pkt.read<uint8_t>();
        int32_t cx, cy;
        cur_pos(std::chrono::steady_clock::now(), cx, cy);
        walk(static_cast<uint16_t>(cx), static_cast<uint16_t>(cy), dstX, dstY, mt);
        portal_armed_ = true;
        portal_dx_ = static_cast<int16_t>(dstX);
        portal_dy_ = static_cast<int16_t>(dstY);
        LOG_DEBUG("GameClientSession %u: portal walk (%d,%d)->(%u,%u)", session_id(), cx, cy, dstX, dstY);
    } catch (...) {}
}

void GameClientSession::walk(uint16_t srcX, uint16_t srcY, uint16_t dstX, uint16_t dstY, uint8_t mt)
{
    // The client chose walk (1) or run (2) — sub_6AF5B0: walk when the run toggle is off or
    // when carrying > 89% of the max WT. The retail move strategy switches the current speed by
    // that type (0 counts as 2) and announces the change (0x01D8) before the move is broadcast.
    const uint8_t mtype = ((mt & 7) == 0) ? 2 : static_cast<uint8_t>(mt & 7);
    if (pcond_test(1)) {
        // sub_75C940: MoveTo (sub_717CB0) refuses under the condition 1 (ESABunkerDo ...) -> S2C 0x0019 [u16 id][pos
        // current][u8 type] to the area (sub_5F52F0) instead of the move.
        int32_t cx, cy;
        cur_pos(std::chrono::steady_clock::now(), cx, cy);
        PacketBuffer h(static_cast<Opcode>(0x0019), is_paula());
        h.write<uint16_t>(static_cast<uint16_t>(object_id_));
        h.write<uint16_t>(static_cast<uint16_t>(cx));
        h.write<uint16_t>(static_cast<uint16_t>(cy));
        h.write<uint8_t>(mtype);
        server_.broadcast_zone(zone_id_, h, 0);
        return;
    }
    apply_move_type_speed(mtype);
    // The server tracks where we ARE, not only where we clicked: the segment src -> dst at our
    // current speed. The engaged monster's follow measures this interpolated position (retail
    // sub_4100C0). pos_x_/pos_y_ = the resting point (destination).
    motion_.start(srcX, srcY, dstX, dstY, cur_speed_, std::chrono::steady_clock::now(), mtype);
    pos_x_ = dstX; pos_y_ = dstY;

    // Moving ends the PLAYER'S own melee (report §3.6) — but NOT the monster's: it keeps its
    // target and follows us until its own follow gives up (ChaseTime / ChaseRange) or its hate
    // entry expires. So we stop only our follow here, not the counter-attack.
    end_attack(true);
    pick_ = Pickup{};                             // a new move leaves the pick-up state (4)

    // (Never re-appear the mover to change its speed: the appear carries position and would snap
    // the avatar. The speed change above is the dedicated 0x01D8, which only sets actor+120.)
    if (object_id_) {
        auto* p = server_.entity_mgr().get_player(object_id_);
        if (p) { p->pos_x = dstX; p->pos_y = dstY; }
    }
    LOG_DEBUG("GameClientSession %u: MOVE (%u,%u)->(%u,%u) type=%u",
              session_id(), srcX, srcY, dstX, dstY, mt);
    // S2C 0x18 MOVE — EXACT 1:1 layout (report §2.4, GameServerRelease sub_5F5240):
    //   [u16 objId][u16 fromX][u16 fromY][u16 toX][u16 toY][u8 type] = 11 bytes, NO padding.
    // ONE echo to self + ONE to observers, block id 0 (dispatched immediately). The old code
    // sent an 18-byte padded self packet TWICE and relied on a 0x34 filler; that double-echo
    // was a mitigation for the self-inflicted "one-behind" (we used to tag every packet with a
    // non-zero block id, so the client held it until the next packet). With block id 0 the
    // single echo is applied at once — the retail server streams exactly one 0x18 per segment.
    PacketBuffer mv(Opcode::C2S_MOVE, is_paula());
    mv.write<uint16_t>(static_cast<uint16_t>(object_id_));   // objId (actor+56)
    mv.write<uint16_t>(srcX); mv.write<uint16_t>(srcY);      // from (cur) pos
    mv.write<uint16_t>(dstX); mv.write<uint16_t>(dstY);      // to (dest) pos
    mv.write<uint8_t>(mt);                                   // move type (1 walk / 2 run / …)
    send(mv);                                               // self echo (commits confirmed pos)
    server_.broadcast_zone(zone_id_, mv, session_id());      // observers walk via pathfinding
    LOG_DEBUG("GameClientSession %u: S2C 0x18 move-OK (eid=%u) %u,%u->%u,%u", session_id(),
              static_cast<uint16_t>(object_id_), srcX, srcY, dstX, dstY);
}

// Zone::Process (sub_810390) -> sub_811DE0, every zone tick: a player whose portal walk is armed
// (+2968), still connected, not leaving (+1641), and not in state 2 is taken through the first
// portal whose rectangle, grown by 24 on every side, holds both its current position and its
// portal destination (sub_422610: x1 <= x < x2 and y1 <= y < y2, int16).
void GameClientSession::portal_tick(std::chrono::steady_clock::time_point now)
{
    const MapGrid* g = server_.zone_grid(zone_id_);
    if (!g || g->portals.empty()) return;
    int32_t px, py;
    cur_pos(now, px, py);
    for (const MapGrid::Portal& pt : g->portals) {
        const int16_t x1 = static_cast<int16_t>(pt.x1 - 24), y1 = static_cast<int16_t>(pt.y1 - 24);
        const int16_t x2 = static_cast<int16_t>(pt.x2 + 24), y2 = static_cast<int16_t>(pt.y2 + 24);
        auto inside = [&](int x, int y) { return x >= x1 && y >= y1 && x < x2 && y < y2; };
        if (inside(static_cast<int16_t>(px), static_cast<int16_t>(py)) && inside(portal_dx_, portal_dy_)) {
            take_portal(pt.zone_to, pt.id);
            return;
        }
    }
}

// The portal move (sub_811DE0 -> +1642 = 1 -> FieldShell vtbl+52 sub_812150 -> sub_7FDD80 ->
// sub_5F7A20): Client::PrepareLeavingZone(zoneTo, changeZoneType 2, pos (0,0), the portal id as
// the connection id, an u8 in the message) and the leave packets (leave_field, npc_talk.cpp).
// Master hands the destination its arrival (changeZoneType 2: the type-1 point of the arrival
// map whose Param is this map and whose Id is the connection id, sub_810CF0) and the client
// moves with 154 -> 156 (begin_field_change).
void GameClientSession::take_portal(int zone_to, uint32_t conn_id)
{
    const int from = zone_id_;
    if (!leave_field(zone_to)) return;
    int32_t ax = 500, ay = 400;
    server_.maps_db().portal_arrival(zone_to, from, static_cast<int>(conn_id & 0xFF), ax, ay);
    LOG_INFO("GameClientSession %u: portal %d -> %d (conn %u) arrival (%d,%d)", session_id(), from, zone_to,
             conn_id & 0xFF, ax, ay);
    begin_field_change(static_cast<int16_t>(zone_to), ax, ay, 2);
}

// Melee is SERVER-DRIVEN (combat_animation_report §2–§3 + client RE): one C2S 0x001D (attack
// request) starts the attacker's battle-follow; the server walks it into range and keeps attacking
// until a stop condition — the client never sends a per-swing packet. C2S 0x0120 is only a
// target-INFO request (answered with 0x0121, no swing). Each swing is a COMMAND BLOCK: its first
// packet 0x001F routes to the client's melee attack action (sub_6EBF20 -> sub_597B70; motion from
// the result byte, no skillId); the block is closed by an empty 0x079F; the monster DEATH is 0x002B
// inside the block. See [[kto-command-block]].
//
// The follow logic is the retail one, 1:1 (GameServerRelease decomp):
//   player  -> LazyBattleFollowingStrategy   process sub_41E470, lazy range sub_41E2E0
//   monster -> NormalBattleFollowingStrategy process sub_41DD40
//   shared  -> order gap sub_41EBC0, order sub_41D940, approach point sub_41E330/sub_41D750,
//              ChaseTime countdown sub_41DA80, attack sub_41DB00, give up vtbl+48 sub_41D6F0.
// Distances are SQUARED distances of CURRENT positions (sub_41ED00) compared with r² (sub_41ED80).
// Not reproduced (no walk grid yet): the Normal variant's walkability test of the approach point
// and the anti-stuck warp in sub_41DB40 — both need the zone map.
static constexpr int      kWorldTickMs     = 100;    // GameTask fps 10 (sub_507F00 -> sub_435FD0(this,10,1))
static constexpr unsigned kChaseGapMs      = 300;    // sub_41EBC0(300, …): chase / re-chase orders
static constexpr unsigned kBackOffGapMs    = 100;    // sub_41EBC0(100, …): back-off orders
static constexpr uint8_t  kReChaseMoveType = 3;      // Normal state 2 re-chase: sub_41EBC0(300, 3) -> chase speed
static constexpr int kHateEntryMs        = 5000;   // hate entry timer: vtbl+124(attacker, 5000, 1) (sub_729E20)

// The attack cooldown, AttackStrategy::DoAttack (sub_41B970): after an attack the next one waits
//   base + 100*DX              (DX >= 0)
//   base * (1 + max(DX,-30)/100) (DX < 0)
// with base from AttackStrategy::Init (sub_41B6E0): a player uses AttackReady + Attack, or Attack2
// after a critical (result 3); a monster always uses Attack. The motion times are the char's .nri
// frame sums (CharRange). DX is the final stat (actor +568).
static int attack_cooldown_ms(const CharRange& r, bool player, uint8_t result, int dx)
{
    int base;
    if (player) base = ((result & 0x7F) == 3) ? r.attack2_ms : (r.attack_ready_ms + r.attack_ms);
    else        base = r.attack_ms;
    if (dx < 0) return static_cast<int>(base * (1.0 + std::max(dx, -30) * 0.01));
    return base + 100 * dx;
}

// In-place resurrect ("Resurrect" button, C2S 0x02F7) is free at or below the beginner level
// (libconfig BeginnerRebirthInfo.Level = 20); above it, it needs an item of ItemParam type 29.
// ("Go to Town", 0x0016, is always available.)
static constexpr int16_t  kBeginnerResurrectLevel = 20;

// rnd / clampi / dist_i / dist2 / cmp_r: combat_util.hpp
// The approach point (sub_41E330 Lazy / sub_41D750 Normal): `radius` (actor +1044 = (Min+Max)/2)
// from the target (tx,ty) toward the follower (fx,fy). If both stand on the same spot, or this is
// the 3rd+ order since the last attack (sub_41EBC0: ++strategy+52 > 2), the direction is random:
// a = rand()%6 radians (an int), offset (cos(a)·r, −sin(a)·r) — the decompiler shows "log" there,
// the disassembly calls _cosf/_sinf (@0x41EA70/@0x41EAB0). The offset is truncated to int
// (sub_40B1F0) and added to the target position (sub_4158E0).
static inline void approach_point(int fx, int fy, int tx, int ty, int radius, bool random_dir,
                                  int& ox, int& oy)
{
    double vx, vy;
    if ((fx == tx && fy == ty) || random_dir) {
        const float a = static_cast<float>(std::rand() % 6);
        vx =  std::cos(a) * radius;
        vy = -std::sin(a) * radius;
    } else {
        const double dx = fx - tx, dy = fy - ty;
        const double d  = std::sqrt(dx * dx + dy * dy);
        vx = dx / d * radius;
        vy = dy / d * radius;
    }
    ox = tx + static_cast<int>(vx);
    oy = ty + static_cast<int>(vy);
}

void GameClientSession::handle_attack(PacketBuffer& pkt, Opcode op)
{
    try {
        if (dead_) return;                            // a dead player can't attack
        if (rest_state_ && static_cast<uint16_t>(op) == 0x001D) return;   // states 8 / 15 swallow it
        if (exchange_ && static_cast<uint16_t>(op) == 0x001D) return;     // and the exchange state 18
        if (compound_ && static_cast<uint16_t>(op) == 0x001D) return;     // and the compound state 13
        // Player case 29: the condition 64 ignores it; with a gun (SubType 54) in slot 1 the melee request is
        // ignored — the gun shoots through C2S 0x01DD (gun.cpp).
        if (static_cast<uint16_t>(op) == 0x001D && pcond_test(64)) return;
        if (static_cast<uint16_t>(op) == 0x001D) head_notice_end();     // the conditions 2 / 3 / 8 end
        if (static_cast<uint16_t>(op) == 0x001D && gun_in_hand()) return;
        uint16_t target_eid = pkt.read<uint16_t>();   // both 0x1D and 0x120 lead with the target eid
        MonsterEntity* m = server_.entity_mgr().get_monster(target_eid);
        if (!m || m->zone != zone_id_) return;        // only monsters in our zone are targetable

        if (static_cast<uint16_t>(op) == 0x0120) {
            // Target-INFO request only: open/refresh the target HP display. Never starts an attack.
            send_target_info(*m);
            return;
        }
        // 0x001D = attack request [u16 target][u8 moveType]: ack it (the local player enters the
        // attack stance on 0x001E), then start (or retarget) the server-driven swing loop. The move
        // type is what the server uses when it walks us into range (report §2.1: 1 walk, 2 run).
        if (!m->alive || m->hp <= 0) return;          // already dead, awaiting removal
        try {
            uint8_t mt = pkt.read<uint8_t>();
            atk_move_type_ = (mt == 0) ? 2 : mt;
            // Evidence for the stored move type (actor+1036): log what the client really sends.
            LOG_DEBUG("GameClientSession %u: attack request eid=%u moveType(raw)=%u", session_id(),
                     target_eid, mt);
        } catch (...) {}
        // BeginAttack (sub_75FA00): already attacking (Player state 1) -> retarget (vtbl+140
        // sub_719210: the target + the strategy restart sub_41B8C0); else vtbl+124 + ChangeState(1),
        // whose entry sends S2C 0x001E to the attacker (empty).
        if (!attacking_) {
            PacketBuffer ack(static_cast<Opcode>(0x001E), is_paula());
            send(ack);
        }
        drill_ = Drill{};                             // ChangeState(1) leaves the drill state 3
        wh_leave_state();                             // and the warehouse state 10
        skill_shop_leave();                           // and the skill master state 25
        start_attack(target_eid);
    } catch (...) {}
}

// S2C 0x0018 MOVE for any entity (player or monster), to everyone in the zone incl. self — the same
// 11-byte layout handle_move already sends: [u16 id][u16 fromX][u16 fromY][u16 toX][u16 toY][u8 type].
// The retail server moves an out-of-range attacker this way (report §2.2/§4.2).
void GameClientSession::broadcast_move(uint16_t eid, int fx, int fy, int tx, int ty, uint8_t type)
{
    PacketBuffer mv(Opcode::C2S_MOVE, is_paula());
    mv.write<uint16_t>(eid);
    mv.write<uint16_t>(static_cast<uint16_t>(fx)); mv.write<uint16_t>(static_cast<uint16_t>(fy));
    mv.write<uint16_t>(static_cast<uint16_t>(tx)); mv.write<uint16_t>(static_cast<uint16_t>(ty));
    mv.write<uint8_t>(type);
    server_.broadcast_zone(zone_id_, mv, 0);
}

void GameClientSession::send_target_info(const MonsterEntity& m)
{
    // S2C 0x0121 (13): [u16 id][i32 HP][i32 MP][u16 gaugeIndex][u8 multiGauge] — requester only.
    PacketBuffer p(static_cast<Opcode>(0x0121), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(m.object_id));
    p.write<int32_t>(m.hp);
    p.write<int32_t>(0);            // monsters carry no MP
    p.write<uint16_t>(0);           // gauge index (0 = ordinary monster)
    p.write<uint8_t>(0);            // multi-gauge
    send(p);
}

// S2C 0x01D8 (3): [u16 id][u8 speed/4] — the actor's current speed changed (retail vtbl+92 ->
// sub_5F70C0, sent to the area only when the speed differs; byte = (int)(speed/4), sub_60C750).
// VERIFIED in the client: the in-world router sub_6F0960 sends 0x1D8 down the same path as 0x0020
// (u16 id -> actor -> sub_47B250) and the actor base handler sub_499010 case 472 sets
// actor+120 = 4*u8 (the step speed) — no re-appear, no snap.
void GameClientSession::broadcast_speed(uint16_t eid, float speed)
{
    PacketBuffer sp(static_cast<Opcode>(0x01D8), is_paula());
    sp.write<uint16_t>(eid);
    sp.write<uint8_t>(static_cast<uint8_t>(static_cast<int>(speed / 4.0f)));
    server_.broadcast_zone(zone_id_, sp, 0);
}

// The player's current speed (retail +132) and its announcement: Player vtbl+92 (sub_760550) sends
// S2C 0x01D8 to the area — including ourselves, so our own client's actor+120 follows — only when
// the value changes.
void GameClientSession::set_current_speed(float speed)
{
    if (speed == cur_speed_) return;
    cur_speed_  = speed;
    move_speed_ = static_cast<uint8_t>(std::min(255, static_cast<int>(speed / 4.0f)));
    if (object_id_) broadcast_speed(static_cast<uint16_t>(object_id_), speed);
}

// PathFindingMoveStrategy::Move (sub_42BC10) for the player: the move's type picks the current
// speed — 1 walk, 2 run (type 0 is treated as 2). Walk = half the run speed (user rule). Types 3/4
// (chase / special +168) have no player value here and keep the current speed.
void GameClientSession::apply_move_type_speed(uint8_t type)
{
    if (spin_speed_ > 0.0f) { set_current_speed(spin_speed_); return; }   // Hammer Spin turning (skill_bear.cpp)
    const float run = run_speed_ * (1.0f + crazy_.speed);                // Crazy mode's Double Crazy (4804)
    if (type == 1)      set_current_speed(run * 0.5f);
    else if (type == 2) set_current_speed(run);
}

// Our CURRENT position (retail sub_4100C0): interpolated while a segment is running, otherwise
// the resting point.
void GameClientSession::cur_pos(std::chrono::steady_clock::time_point now, int32_t& x, int32_t& y) const
{
    if (motion_.moving(now)) { motion_.pos(now, x, y); return; }
    x = pos_x_; y = pos_y_;
}

// C2S 0x001D: the player's attack strategy starts its battle-follow on the target. Init
// (sub_41E2B0 -> sub_41D5F0: budget = ChaseTime, orders = 0, no order yet) then "follow" (vtbl+36
// sub_41D660 -> state 1). The first decision runs on the next world tick, as on the retail server.
// The NormalAttackStrategy (re)start: sub_41B890 (start) / sub_41B8C0 (retarget, which also stops the
// follow) set +60 = 1 — the strategy enters its attack state once its cooldown (+56) is over, and
// that entry (sub_41BE70 case 1, shared with the gun) sends S2C 0x0019 halt to the area, stops our
// move strategy and starts the follow from state 1 (melee_enter_).
void GameClientSession::start_attack(uint16_t target_eid)
{
    if (dead_) return;
    pick_ = Pickup{};                     // attacking leaves the pick-up state (4)
    atk_target_ = target_eid;
    attacking_  = true;
    pf_ = BattleFollow{};
    pf_.state     = 0;                     // the follow waits for the strategy's entry
    pf_.budget_ms = 0x7FFFFFFF;            // Character init: actor+1056 = 0x7FFFFFFF (players never time out)
    melee_enter_  = true;
    ensure_world_tick();
}

void GameClientSession::end_attack(bool send_leave)
{
    if (!attacking_) return;
    attacking_ = false;
    pf_.state  = 0;
    gun_start_ = gun_stop_ = false;
    melee_enter_ = false;
    if (send_leave && object_id_) {
        // S2C 0x0020 (2): [u16 id] — the attacker leaves its attack state (block 0, area).
        PacketBuffer lv(static_cast<Opcode>(0x0020), is_paula());
        lv.write<uint16_t>(static_cast<uint16_t>(object_id_));
        server_.broadcast_zone(zone_id_, lv, 0);
    }
}

// The monster we hit takes us as its target (retail: the damage event 30002 posts 30001 to the
// monster, CommonMonster state 0 -> vtbl+124(attacker, 5000, 1) + state 1 (combat) -> its attack
// strategy starts its battle-follow). A monster already fighting someone else keeps its target (the
// first entry of its hate list); one fighting us gets its hate entry set back to 5000 ms.
void GameClientSession::engage_monster(MonsterEntity& m)
{
    const uint16_t eid = static_cast<uint16_t>(m.object_id);
    for (MobEngage& e : mobs_)
        if (e.eid == eid) { e.hate_ms = kHateEntryMs; return; }
    if (m.target_oid && m.target_oid != object_id_ && server_.player_session(m.target_oid)) return;
    const bool begins = (m.target_oid != object_id_);   // (not already ours: its battle state 1 begins)
    m.target_oid = object_id_;
    if (begins) mon_speech_now(m, 1);                    // CommonMonster sub_729E20 state 1 enter: its line (S2C 184)
    MobEngage e;
    e.eid          = eid;
    e.hate_ms      = kHateEntryMs;
    e.mf.state     = 1;
    e.mf.budget_ms = m.chase_time_ms;      // CommonMonster::Initialize: actor+1056 = param ChaseTime
    mobs_.push_back(e);
    // Its follow evaluates on the next world tick; its attack cooldown (+56) starts ready
    // (AttackStrategy::Init sub_41B6E0), so it can retaliate at once when in range.
    ensure_world_tick();
}

// An aggressive monster found us (AggressiveAttackStrategy sub_424660 -> 30001 to itself:
// vtbl+124(us, 5000, 1) + combat). Posted to our strand by the monster AI (monster_ai.cpp), which
// already marked the monster with our id.
void GameClientSession::engage_by_aggro(uint16_t eid)
{
    MonsterEntity* m = server_.entity_mgr().get_monster(eid);
    if (!m) return;
    if (dead_ || leaving_ || state_ != GameClientSessionState::InWorld || m->zone != zone_id_ || !m->alive) {
        if (m->target_oid == object_id_) m->target_oid = 0;           // nobody to fight: idle again
        return;
    }
    m->target_oid = 0;                                                // (engage_monster sets it)
    engage_monster(*m);
    LOG_DEBUG("GameClientSession %u: aggressive mob %u (%s) attacks us", session_id(), eid, m->name.c_str());
}

bool GameClientSession::engaged_with(uint16_t eid) const
{
    for (const MobEngage& e : mobs_) if (e.eid == eid) return true;
    return false;
}

void GameClientSession::ensure_world_tick()
{
    if (ticking_) return;
    ticking_   = true;
    last_tick_ = std::chrono::steady_clock::now();
    const uint32_t g = ++tick_gen_;
    auto self = shared_from_this();
    schedule_delayed(std::chrono::milliseconds(kWorldTickMs), [this, self, g]() { world_tick(g); });
}

// One retail world tick for this player (GameTask::Process -> world.Process(elapsed) -> Player
// OnProcessState): HP/MP recovery, then the player's battle-follow, then the engaged monster's.
// Runs for as long as the player is in the world.
void GameClientSession::world_tick(uint32_t gen)
{
    if (gen != tick_gen_) return;
    if (state_ != GameClientSessionState::InWorld) { ticking_ = false; return; }
    const auto now = std::chrono::steady_clock::now();
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick_).count());
    last_tick_ = now;
    // The next tick is booked first (GSC-10): a step that throws (e.g. a DB error in the post sync) reaches the io
    // loop's catch, and booked last it stopped this player's world for good (ticking_ stayed set).
    auto self = shared_from_this();
    schedule_delayed(std::chrono::milliseconds(kWorldTickMs), [this, self, gen]() { world_tick(gen); });

    if (cdm_.active)              cdm_tick(elapsed);       // the Crazy Drill Mode (sub_7AFE50, every tick)
    if (mqs_.active && !dead_)    monq_tick(elapsed);      // the monster quest's timer (sub_75E780)
    if (pq_.qid && !dead_)        pq_tick(elapsed);        // the party quest's timer (sub_75E5B0)
    if (!dead_)                   regen_tick(elapsed);     // sub_73CF10
    party_tick(elapsed);                                   // the party's view of us (party.cpp)
    if (!dead_)                   protect_tick(elapsed);   // sub_73CFC0
    if (!es_.empty())             es_tick(elapsed);
    if (!cast_es_.empty())        cast_es_tick(elapsed);   // our skills' effect objects on others
    if (crazy_.do_id)             crazy_tick(elapsed);     // the Bear's Crazy mode (skill_bear.cpp)
    post_tick(elapsed);                                    // the post box's sync (postbox.cpp, sub_76BF70)
    if (cd_ratio_ != 1.0f && !skill_cd_.empty()) {         // sub_88F9C0: each cool time runs (int)(dt * the ratio)
        const int run = static_cast<int>(static_cast<double>(elapsed) * static_cast<double>(cd_ratio_));
        for (auto& c : skill_cd_) c.second -= std::chrono::milliseconds(run - elapsed);
    }
    if (attacking_ && !dead_)     player_follow_tick(now);
    if (su_.active && !dead_)     skill_tick(now, elapsed);
    if (pick_.active && !dead_)   pickup_tick(now);
    if (drill_.active && !drill_.started && !dead_) drill_tick(now);   // drill.cpp: the walk ended -> 30008
    if (!mobs_.empty() && !dead_) mob_follow_tick(now, elapsed);
    if (portal_armed_ && !dead_ && !leaving_) portal_tick(now);
    // sub_73D2A0's tick, every state: MP <= MaxMP * 0.3f -> our pet's line 7 (dbl_995F20 = (double)0.3f)
    if (pet_.active && static_cast<double>(mp_max_) * 0.30000001192092896 >= static_cast<double>(mp_)) pet_speech(7);
    if (pet_.active && !leaving_) pet_tick(now, elapsed);   // our pet's process (pet.cpp)
}

// Player HP/MP recovery, retail Player tick (sub_73D2A0 -> sub_73CF10, skipped while dead):
//   while the recovery pause (+3188) runs, only count it down (taking a hit sets it to 6000 ms,
//   sub_73CEF0 from OnDamaged); otherwise every 2000 ms (sub_75DFF0 / sub_75E140, the first one at
//   once) add the recovery rate (+3168 HP / +3180 MP) plus the buff bonus (0 without buffs) and send
//   S2C 0x24 [u32 HP] / 0x25 [u32 MP] to ourselves; at full only up to 3 sends in a row.
// Rates (sub_7654A0, set on entering the state 0 / 8 / 15): 5 / 5 in the normal state; sitting
// (8) and resting (15) by the FType (+1028), HP / MP:
//   Power 15/10, 30/20 · Magic 10/15, 20/30 · Sense 15/15, 30/30 · Charm 15/10, 30/20.
static constexpr int kRegenIntervalMs = 2000;   // sub_73CF10: sub_75DFF0(elapsed, 2000)
static constexpr int kRegenPauseMs    = 6000;   // sub_73CEF0: +3188 = 6000
static constexpr int kRegenRateNormal = 5;      // sub_7654A0: every FType, normal state
static constexpr int kRegenRateRest[4][2][2] = {   // [FType][sitting, resting][HP, MP]
    { { 15, 10 }, { 30, 20 } }, { { 10, 15 }, { 20, 30 } }, { { 15, 15 }, { 30, 30 } }, { { 15, 10 }, { 30, 20 } } };

void GameClientSession::regen_tick(int elapsed_ms)
{
    int hp_rate = kRegenRateNormal, mp_rate = kRegenRateNormal;          // +3168 / +3180
    if ((rest_state_ == 8 || rest_state_ == 15) && f_type_ < 4) {       // (another FType: logged, kept)
        const int r = (rest_state_ == 15) ? 1 : 0;
        hp_rate = kRegenRateRest[f_type_][r][0];
        mp_rate = kRegenRateRest[f_type_][r][1];
    }
    if (regen_pause_ms_ > 0) {
        regen_pause_ms_ -= elapsed_ms;
        if (regen_pause_ms_ < 0) regen_pause_ms_ = 0;
        return;
    }
    // S2C 0x24 / 0x25: VERIFIED in the client — routed to the local player (sub_6F0960 cases 36/37
    // -> sub_47B250) and applied by sub_6B4E00: case 0x24 sets cur HP (+452) = u32 clamped to the
    // max (+496); case 0x25 sets cur MP (+456) clamped to +472.
    auto send_value = [&](uint16_t op, int32_t v) {
        PacketBuffer p(static_cast<Opcode>(op), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(v));
        send(p);
    };
    // The next recovery's time (sub_716FE0): a Homo party's PartyRecoveryTimeAccelator takes 0.8 of it
    // (sub_86D510).
    const int interval = (party_bonus_ == 1) ? static_cast<int>(static_cast<double>(kRegenIntervalMs) * 0.800000011920929)
                                             : kRegenIntervalMs;
    hp_regen_ms_ -= elapsed_ms;
    if (hp_regen_ms_ <= 0) {
        hp_regen_ms_ = interval;
        if (hp_ >= hp_max_) {
            if (hp_full_sends_ < 3) { ++hp_full_sends_; send_value(0x0024, hp_); }
        } else {
            hp_ = std::min(hp_ + regen_amount(0, hp_rate), hp_max_);   // ResChain 2 (sub_717030)
            hp_full_sends_ = 0;
            send_value(0x0024, hp_);
        }
    }
    mp_regen_ms_ -= elapsed_ms;
    if (mp_regen_ms_ <= 0) {
        mp_regen_ms_ = interval;
        if (mp_ >= mp_max_) {
            if (mp_full_sends_ < 3) { ++mp_full_sends_; send_value(0x0025, mp_); }
        } else {
            mp_ = std::min(mp_ + regen_amount(1, mp_rate), mp_max_);   // + Aura of Mana (sub_557100)
            mp_full_sends_ = 0;
            send_value(0x0025, mp_);
        }
    }
}

// sub_41EBC0 + sub_41D940 for the PLAYER: an order is issued only if `gap` ms passed since the
// previous one (none yet = ready). It walks us from our current position to the approach point
// of the target's current position, with the strategy's stored move type (the C2S 0x001D byte),
// whose speed the move strategy applies first (0x01D8 on a change), and announces it with S2C
// 0x0018 (sub_5F5240: [id][cur][dest][type]).
bool GameClientSession::player_order(MonsterEntity& m, unsigned gap_ms, uint8_t type,
                                     std::chrono::steady_clock::time_point now)
{
    if (pf_.ordered && now - pf_.last_order < std::chrono::milliseconds(gap_ms)) return false;
    pf_.ordered    = true;
    pf_.last_order = now;
    const bool random_dir = ++pf_.orders > 2;

    int32_t px, py, mx, my;
    cur_pos(now, px, py);
    m.motion.pos(now, mx, my);
    const CharRange rg = attack_range();
    int ox = 0, oy = 0;
    approach_point(px, py, mx, my, rg.approach(), random_dir, ox, oy);

    const uint8_t mtype = ((type & 7) == 0) ? 2 : static_cast<uint8_t>(type & 7);   // sub_42BC10: 0 -> 2
    apply_move_type_speed(mtype);
    motion_.start(px, py, ox, oy, cur_speed_, now, mtype);
    pos_x_ = ox; pos_y_ = oy;
    if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = ox; p->pos_y = oy; }
    broadcast_move(static_cast<uint16_t>(object_id_), px, py, ox, oy, type);
    LOG_DEBUG("GameClientSession %u: follow(state %d) eid=%u d=%d range=[%d,%d] -> (%d,%d) %dms",
             session_id(), pf_.state, atk_target_, dist_i(px, py, mx, my), rg.min_atk, rg.max_atk,
             ox, oy, motion_.dur_ms);
    return true;
}

// sub_41EBC0 + sub_41D940 for the MONSTER. The order's move type sets the current speed first
// (PathFindingMoveStrategy sub_42BC10: type 0 is treated as 2, then 1 walk +156, 2 run +160,
// 3 chase +164) and a change is announced with 0x01D8 before the 0x0018 move — which still carries
// the order's own type (sub_5F5240 gets the unconverted value; the client also maps 0 to 2).
bool GameClientSession::mob_order(MonsterEntity& m, BattleFollow& mf, unsigned gap_ms, uint8_t type,
                                  std::chrono::steady_clock::time_point now)
{
    if (mf.ordered && now - mf.last_order < std::chrono::milliseconds(gap_ms)) return false;
    mf.ordered    = true;
    mf.last_order = now;
    const bool random_dir = ++mf.orders > 2;
    // MoveTo (sub_717CB0) refuses a monster under the condition 1 (held): the order fails, no halt
    // (sub_41D940 halts only a mover that is not held).
    if (m.cond_test(1)) return false;

    const uint8_t mtype = ((type & 7) == 0) ? 2 : static_cast<uint8_t>(type & 7);
    float speed = m.cur_speed;
    if      (mtype == 1) speed = m.walk_speed;
    else if (mtype == 2) speed = m.run_speed;
    else if (mtype == 3) speed = m.chase_speed;
    if (speed != m.cur_speed) {
        m.cur_speed  = speed;
        m.move_speed = static_cast<uint8_t>(std::min(255, static_cast<int>(speed / 4.0f)));
        broadcast_speed(static_cast<uint16_t>(m.object_id), speed);
    }

    int32_t mx, my, px, py;
    m.motion.pos(now, mx, my);
    cur_pos(now, px, py);
    const CharRange mr = server_.char_range().get(m.species);
    int ox = 0, oy = 0;
    approach_point(mx, my, px, py, mr.approach(), random_dir, ox, oy);

    m.motion.start(mx, my, ox, oy, m.cur_speed, now, mtype);
    m.pos_x = ox; m.pos_y = oy;
    broadcast_move(static_cast<uint16_t>(m.object_id), mx, my, ox, oy, type);
    LOG_DEBUG("GameClientSession %u: mob %u follow(state %d) d=%d range=[%d,%d] -> (%d,%d) type=%u speed=%.1f %dms",
             session_id(), static_cast<unsigned>(m.object_id), mf.state, dist_i(mx, my, px, py),
             mr.min_atk, mr.max_atk, ox, oy, type, m.cur_speed, m.motion.dur_ms);
    return true;
}

// The PLAYER: LazyBattleFollowingStrategy::Process (sub_41E470).
//   lazy range (sub_41E2E0) = min((int)(Max*1.5), Chase)
//   state 1 (before the first attack): d <= lazy -> attack; else chase order (gap 300, stored type)
//           -> state 5, or give up if it can't be issued.
//   state 2 (after an attack): d > Chase -> give up; d > Max -> re-chase (gap 300, stored type)
//           -> state 4; d >= Min, or the target is targeting us -> attack; else back off
//           (gap 100) -> state 3. An order that can't be issued is retried next tick.
//   states 3/4/5: moving; on arrival (event 30008) 3/4 -> 2, 5 -> 1.
//   state 6: the attack was requested (30013); the attack strategy fires it once its cooldown
//            (+56) allows (sub_41BD80 -> DoAttack sub_41B970), which re-arms the cooldown and puts
//            the follow back in state 2 (vtbl+52 sub_41D6D0) — so a target that walked away is
//            re-chased on the next tick, not after the cooldown.
// The budget countdown is skipped: a player's ChaseTime is 0x7FFFFFFF.
void GameClientSession::player_follow_tick(std::chrono::steady_clock::time_point now)
{
    MonsterEntity* m = server_.entity_mgr().get_monster(atk_target_);
    if (!m || m->zone != zone_id_ || !m->alive || m->hp <= 0) { end_attack(true); return; }
    if (gun_in_hand()) { gun_follow_tick(*m, now); return; }   // GunAttackStrategy + Normal follow

    if (melee_enter_) {                                 // sub_41BD80 case 1 -> sub_41BE70 case 1
        if (now < pl_atk_ready_) return;                // the cooldown (+56) still runs
        melee_enter_ = false;
        int32_t cx, cy;
        cur_pos(now, cx, cy);
        const uint32_t moving = motion_.state_flags(now);
        PacketBuffer h(static_cast<Opcode>(0x0019), is_paula());       // sub_5F52F0
        h.write<uint16_t>(static_cast<uint16_t>(object_id_));
        h.write<uint16_t>(static_cast<uint16_t>(cx));
        h.write<uint16_t>(static_cast<uint16_t>(cy));
        h.write<uint8_t>(moving ? static_cast<uint8_t>(moving) : 1);
        server_.broadcast_zone(zone_id_, h, 0);
        motion_.rest(cx, cy);                                            // sub_41C5D0
        pos_x_ = cx; pos_y_ = cy;
        if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = cx; p->pos_y = cy; }
        pf_ = BattleFollow{};
        pf_.state     = 1;                                               // the follow (vtbl+36)
        pf_.budget_ms = 0x7FFFFFFF;
        return;
    }
    if (pf_.state == 3 || pf_.state == 4 || pf_.state == 5) {
        if (motion_.moving(now)) return;
        pf_.state = (pf_.state == 5) ? 1 : 2;           // event 30008: the ordered move arrived
    }
    auto fire = [&]() {                                 // state 6: DoAttack when the cooldown allows
        if (now < pl_atk_ready_) return;
        pf_.state = 2;
        const uint8_t result = player_swing(*m, now);
        pl_atk_ready_ = now + std::chrono::milliseconds(       // (the Bear's Crazy mode: DX -30, skill_bear.cpp)
            attack_cooldown_ms(server_.char_range().get(type_), /*player*/true, result,
                               crazy_.do_id ? -crazy_.dx : st_dx_));
    };
    if (pf_.state == 6) { fire(); return; }

    int32_t px, py, mx, my;
    cur_pos(now, px, py);
    m->motion.pos(now, mx, my);
    const int64_t d2 = dist2(px, py, mx, my);
    const CharRange rg = attack_range();

    auto attack = [&]() {                               // sub_41DB00
        pf_.budget_ms = 0x7FFFFFFF;
        pf_.orders    = 0;
        pf_.state     = 6;
        fire();
    };

    if (pf_.state == 1) {
        const int lazy = std::min(static_cast<int>(rg.max_atk * 1.5), rg.chase);
        if (cmp_r(d2, lazy) <= 0)                                   { attack(); return; }
        if (player_order(*m, kChaseGapMs, atk_move_type_, now))     { pf_.state = 5; return; }
        end_attack(true);                                           // vtbl+48: give up
        return;
    }
    if (pf_.state == 2) {
        if (cmp_r(d2, rg.chase) > 0)    { end_attack(true); return; }   // beyond ChaseRange: give up
        if (cmp_r(d2, rg.max_atk) > 0) {
            if (player_order(*m, kChaseGapMs, atk_move_type_, now)) pf_.state = 4;
            return;
        }
        const bool targets_us = engaged_with(atk_target_);
        if (cmp_r(d2, rg.min_atk) >= 0 || targets_us) { attack(); return; }
        if (player_order(*m, kBackOffGapMs, atk_move_type_, now)) pf_.state = 3;
    }
}

// The player with a gun in slot 1 (gun.cpp): GunAttackStrategy (5) + NormalBattleFollowingStrategy
// (1), with the gun's ranges (attack_range).
//   The attack strategy first: a fired shot stops it now (+60 = 3 -> vtbl+40 sub_41B910: state 0,
//   the follow stops, 30009 -> the player leaves the attack state: S2C 0x0020); a fresh start enters
//   its attack state (sub_41BE70 case 1): S2C 0x0019 [id][pos][type] (the move bits, 0 -> 1) to the
//   area, stop, the follow from state 1 (vtbl+36 sub_41D660).
//   Then the follow (sub_41DD40, the player's ChaseTime 0x7FFFFFFF never runs out):
//   state 1: d <= Max: d >= Min -> attack; nearer -> back off (gap 100, stored type) -> 3 (a refused
//            order is retried; sub_41DB40 only moves monsters); d > Max -> chase (gap 300, stored
//            type) -> 5, else give up;
//   state 2: d > Chase -> give up; d > Max -> re-chase (gap 300, type 3) -> 4; else as state 1;
//   states 3/4/5: moving; on arrival 3/4 -> 2, 5 -> 1; state 6: waiting for the shot.
//   attack (sub_41DB00): orders = 0, state 6, 30013 -> the strategy fires DoAttack (no cooldown).
void GameClientSession::gun_follow_tick(MonsterEntity& m, std::chrono::steady_clock::time_point now)
{
    if (gun_stop_) { gun_stop_ = false; end_attack(true); return; }
    if (gun_start_) {
        gun_start_ = false;
        int32_t cx, cy;
        cur_pos(now, cx, cy);
        const uint32_t moving = motion_.state_flags(now);
        PacketBuffer h(static_cast<Opcode>(0x0019), is_paula());       // sub_5F52F0
        h.write<uint16_t>(static_cast<uint16_t>(object_id_));
        h.write<uint16_t>(static_cast<uint16_t>(cx));
        h.write<uint16_t>(static_cast<uint16_t>(cy));
        h.write<uint8_t>(moving ? static_cast<uint8_t>(moving) : 1);
        server_.broadcast_zone(zone_id_, h, 0);
        motion_.rest(cx, cy);                                            // sub_41C5D0
        pos_x_ = cx; pos_y_ = cy;
        if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = cx; p->pos_y = cy; }
        pf_ = BattleFollow{};
        pf_.state = 1;
    }
    if (pf_.state == 3 || pf_.state == 4 || pf_.state == 5) {
        if (motion_.moving(now)) return;
        pf_.state = (pf_.state == 5) ? 1 : 2;                            // event 30008: arrived
    }
    if (pf_.state != 1 && pf_.state != 2) return;                        // idle / waiting for the shot

    int32_t px, py, mx, my;
    cur_pos(now, px, py);
    m.motion.pos(now, mx, my);
    const int64_t d2 = dist2(px, py, mx, my);
    const CharRange rg = attack_range();
    const bool can_move = run_speed_ > 0.0f;                             // +156 / +160 > 0

    auto attack = [&]() {
        pf_.orders = 0;
        pf_.state  = 6;
        if (gun_shot(m, now) && attacking_) gun_stop_ = true;            // vtbl+48 sub_41B7A0: +60 = 3
    };
    if (pf_.state == 2 && cmp_r(d2, rg.chase) > 0) { end_attack(true); return; }
    if (cmp_r(d2, rg.max_atk) <= 0) {
        if (cmp_r(d2, rg.min_atk) >= 0 || !can_move) { attack(); return; }
        if (player_order(m, kBackOffGapMs, atk_move_type_, now)) pf_.state = 3;
        return;
    }
    if (pf_.state == 1) {
        if (player_order(m, kChaseGapMs, atk_move_type_, now)) pf_.state = 5;
        else end_attack(true);
        return;
    }
    if (player_order(m, kChaseGapMs, kReChaseMoveType, now)) pf_.state = 4;
}

// The MONSTER: NormalBattleFollowingStrategy::Process (sub_41DD40), with its ChaseTime budget
// (strategy+60 <- actor+1056 = MonsterParamEx2.ChaseTime ms): counted down on every tick while it
// decides (states 1/2) or moves (3/4/5); reset by every attack (sub_41DB00); at 0 it gives up.
//   state 1: d <= Max: d >= Min -> attack; too close -> back off (gap 100, stored type 0) -> 3,
//            or attack anyway if it can't move (speeds 0); d > Max -> chase (gap 300, type 0)
//            -> 5, else give up.
//   state 2: d > Chase -> give up; d > Max -> re-chase (gap 300, type 3 = chase speed) -> 4;
//            else as state 1 (a failed order is retried next tick).
// The hate entry (5000 ms, refreshed by our swings) still bounds the whole engagement.
void GameClientSession::mob_follow_tick(std::chrono::steady_clock::time_point now, int elapsed_ms)
{
    for (size_t i = 0; i < mobs_.size(); ) {
        const uint16_t eid = mobs_[i].eid;
        if (!mob_follow_one(mobs_[i], now, elapsed_ms)) continue;   // it ended (and left the list)
        if (dead_) return;                                          // we died: every engagement is over
        if (i < mobs_.size() && mobs_[i].eid == eid) ++i;
    }
}

// One monster's follow; false = its engagement ended (removed from mobs_).
bool GameClientSession::mob_follow_one(MobEngage& e, std::chrono::steady_clock::time_point now, int elapsed_ms)
{
    const uint16_t eid = e.eid;
    BattleFollow& mf = e.mf;
    MonsterEntity* m = server_.entity_mgr().get_monster(eid);
    if (!m || m->zone != zone_id_ || !m->alive || m->hp <= 0) { end_mob_attack(eid, false); return false; }
    e.hate_ms -= elapsed_ms;                                    // sub_5E9540
    if (e.hate_ms <= 0) { end_mob_attack(eid, true); return false; }

    if (mf.state == 3 || mf.state == 4 || mf.state == 5) {
        if (m->motion.moving(now)) {
            mf.budget_ms -= elapsed_ms;                         // sub_41DA80 (LABEL_77)
            if (mf.budget_ms <= 0) {
                LOG_DEBUG("GameClientSession %u: mob %u gives up (ChaseTime)", session_id(), eid);
                end_mob_attack(eid, true);
                return false;
            }
            return true;
        }
        mf.state = (mf.state == 5) ? 1 : 2;                     // event 30008: arrived
    }
    auto fire = [&]() {                                         // state 6: DoAttack when the cooldown allows
        if (now < m->atk_ready) return;
        // The monster strategies' DoAttack (sub_423770) first: a hidden target player that none of its
        // ESAOpticalEyes (+456 — monsters have none) sees, then a shadow monster whose target player has
        // no Third Eye, stops it (vtbl+40 sub_423740 -> sub_41B910: idle, event 30009).
        if (hiding_) {
            LOG_DEBUG("GameClientSession %u: mob %u stops — its target is hidden", session_id(), eid);
            end_mob_attack(eid, true);
            return;
        }
        if (m->shadow() && !third_eye_) {
            LOG_DEBUG("GameClientSession %u: shadow mob %u stops — no Third Eye", session_id(), eid);
            end_mob_attack(eid, true);
            return;
        }
        mf.state = 2;
        const uint8_t result = mob_swing(*m, now);
        m->atk_ready = now + std::chrono::milliseconds(
            attack_cooldown_ms(server_.char_range().get(m->species), /*player*/false, result, m->dx));
    };
    if (mf.state == 6) { fire(); return engaged_with(eid); }

    mf.budget_ms -= elapsed_ms;                                 // sub_41DA80
    if (mf.budget_ms <= 0) {
        LOG_DEBUG("GameClientSession %u: mob %u gives up (ChaseTime)", session_id(), eid);
        end_mob_attack(eid, true);
        return false;
    }

    int32_t mx, my, px, py;
    m->motion.pos(now, mx, my);
    cur_pos(now, px, py);
    const int64_t d2 = dist2(mx, my, px, py);
    const CharRange mr = server_.char_range().get(m->species);
    const bool can_move = m->walk_speed > 0.0f && m->run_speed > 0.0f;

    auto attack = [&]() {                                       // sub_41DB00
        mf.budget_ms = m->chase_time_ms;
        mf.orders    = 0;
        mf.state     = 6;
        fire();
    };
    auto give_up = [&]() {
        LOG_DEBUG("GameClientSession %u: mob %u gives up (d=%d chase=%d)", session_id(), eid,
                 dist_i(mx, my, px, py), mr.chase);
        end_mob_attack(eid, true);
    };

    if (mf.state == 2 && cmp_r(d2, mr.chase) > 0) { give_up(); return false; }
    if (cmp_r(d2, mr.max_atk) <= 0) {
        if (cmp_r(d2, mr.min_atk) >= 0 || !can_move) { attack(); return engaged_with(eid); }
        if (mob_order(*m, mf, kBackOffGapMs, /*stored type*/0, now)) mf.state = 3;
        return true;
    }
    if (mf.state == 1) {
        if (mob_order(*m, mf, kChaseGapMs, /*stored type*/0, now)) { mf.state = 5; return true; }
        give_up();
        return false;
    }
    if (mob_order(*m, mf, kChaseGapMs, kReChaseMoveType, now)) mf.state = 4;
    return true;
}

// One player melee swing (the attack the follow strategy triggers in state 6). Returns the result
// byte (its crit bit picks the next cooldown).
uint8_t GameClientSession::player_swing(MonsterEntity& mref, std::chrono::steady_clock::time_point now)
{
    MonsterEntity* m = &mref;
    const uint16_t target_eid = atk_target_;
    // Retail basic melee (DoAttack sub_41B970 -> sub_5E5700(a5 = 1 for a player)), player -> monster.
    // The rolls, all made, in this order: hit (sub_5E43E0), the monster's evade (sub_5E4500) and
    // block (sub_5E4610). Then a crit charged by an earlier miss (+1032 = atk_power_: 3 crit, 4 strong) lands
    // whatever the rolls; else a hit neither evaded nor blocked is a crit or a normal hit (crit_roll /
    // strong_roll); a missed swing may charge the next one's crit (sub_7169F0 / sub_716930).
    // result byte: 0 hit, 1 miss/evade (the start value), 2 block, 3 crit, 4 strong crit; |0x80 fatal
    // — from the damage BEFORE the resistance, which sub_5E6510 then takes (not after a charged
    // plain crit: that branch skips sub_5E6510, retail).
    uint8_t result = 1;
    int32_t dmg = 0;
    const int ap = crazy_swing_ap();                     // the Bear's Crazy mode: + (base + items) * Ratio (skill_bear.cpp)
    bool hit   = melee_hit_player(st_ac_);
    bool evade = rnd(1000) < static_cast<int>(std::min(m->hv * 0.2000000029802322, 20.0) * 10.0);
    bool block = rnd(1000) < static_cast<int>(std::min(m->lk * 0.5, 50.0) * 10.0);
    if (m->cond_test(16)) { hit = true; evade = false; block = false; }   // the target's condition 16 (Guard
                                                                           // Break): it neither dodges nor blocks
    // DamageInfo [4]: 0x400 under our Chi Sword (+0x148 flag 64); a frozen target (its +0x148 flag 32,
    // ESAChillingPrison) — after the rolls, before a charged crit — is a miss with 0x200.
    int32_t dinfo_flags = chi_sword_on() ? 0x400 : 0;
    const bool frozen = (m->skill_flags & 32) != 0;
    if (frozen) dinfo_flags |= 0x200;
    bool resist = false;
    if (frozen) {
        // result 1, no damage
    } else if (atk_power_ == 3) {
        atk_power_ = 0;
        dmg = melee_crit_damage(ap, m->dp);
        result = (dmg < m->hp) ? 3 : 131;
    } else if (atk_power_ == 4) {
        atk_power_ = 0;
        dmg = melee_strong_damage(ap, m->dp);
        result = (dmg < m->hp) ? 4 : 132;
        resist = true;
    } else if (hit) {
        if (!evade) {
            if (block) {
                result = 2;
            } else {
                if (crit_roll(st_lk_ + crazy_.crit)) {           // (Crazy mode: + 4908 CriticalRatio)
                    if (strong_roll(st_ac_)) { dmg = melee_strong_damage(ap, m->dp); result = 4; }
                    else                     { dmg = melee_crit_damage(ap, m->dp);   result = 3; }
                } else {
                    dmg = melee_damage(ap, m->dp);
                    result = 0;
                }
                if (dmg >= m->hp) result |= 0x80;
                resist = true;
            }
        }
    } else if (rnd(100) < std::min(st_lk_ + 50, 90)) {                  // sub_7169F0
        atk_power_ = (st_ac_ > m->hv + 60) ? 4 : 3;                   // sub_716930
    }
    if (resist) {
        // sub_5E6510: the target's PhysicalR (slot 20) takes its share of the damage.
        int32_t absorbed = 0;
        dmg = melee_resist(dmg, m->resist_slot(20), absorbed);
        if (absorbed) absorb_heal(*m, absorbed);         // event 30182 (PhysicalR > 100)
    }
    hide_break();                                        // sub_41B970: a hidden player's swing ends its hide

    // The share of the monster's EXP for this swing is taken before the damage event (vtbl+264); a
    // damaging hit marks us as the owner when it has none (the monster's 30002 handler sub_423B70).
    const int32_t hp_before = m->hp;
    const int32_t swing_dmg = dmg;                           // the swing (0x001F) shows the blow's
    if (dinfo_flags == 0 && m->es_on(1320)) {                // ESAHarden's hook (sub_57F420): half, flag 1
        dmg = static_cast<int32_t>(static_cast<double>(dmg) * 0.5);
        dinfo_flags = 1;
    }
    if (dmg > 0) mark_monster_owner(*m);
    int32_t hp = m->hp, mx = m->hp_max;
    bool killed = false;
    if (dmg > 0) { killed = server_.entity_mgr().damage_monster(target_eid, dmg, hp, mx); if (hp < 0) hp = 0; }

    const uint16_t self_eid = static_cast<uint16_t>(object_id_);
    int32_t cx, cy, mcx, mcy;                              // CURRENT positions (sub_4100C0)
    cur_pos(now, cx, cy);
    m->motion.pos(now, mcx, mcy);
    const uint16_t ax = static_cast<uint16_t>(cx),  ay = static_cast<uint16_t>(cy);
    const uint16_t tx = static_cast<uint16_t>(mcx), ty = static_cast<uint16_t>(mcy);

    LOG_DEBUG("GameClientSession %u: swing eid=%u d=%d dmg=%d -> hp=%d/%d%s",
             session_id(), target_eid, dist_i(cx, cy, mcx, mcy), dmg, hp, mx, killed ? " [KILLED]" : "");

    // 0x001B (7) — swing announcement, block 0, area (incl. attacker). [u16 id][u16 x][u16 y][u8 2]
    {
        PacketBuffer a(static_cast<Opcode>(0x001B), is_paula());
        a.write<uint16_t>(self_eid);
        a.write<uint16_t>(ax); a.write<uint16_t>(ay);
        a.write<uint8_t>(2);
        server_.broadcast_zone(zone_id_, a, 0);
    }

    // The command block: 0x001F (first, the swing) + 0x002A (damage number + HP drop) +
    // 0x011E (monster HP after the hit) [+ 0x002B on a kill]. broadcast_block stamps one shared
    // id, sends the group then the CLOSE (0x079F) to each recipient.
    std::vector<PacketBuffer> group;
    // 0x001F (17): [u16 atk][u16 kind=1][u16 tgt][u16 ax][u16 ay][u16 tx][u16 ty][i16 dmg][u8 result]
    {
        PacketBuffer s(static_cast<Opcode>(0x001F), is_paula());
        s.write<uint16_t>(self_eid);
        s.write<uint16_t>(1);                        // kind = 1 (melee)
        s.write<uint16_t>(target_eid);
        s.write<uint16_t>(ax); s.write<uint16_t>(ay);
        s.write<uint16_t>(tx); s.write<uint16_t>(ty);
        s.write<int16_t>(static_cast<int16_t>(swing_dmg));
        s.write<uint8_t>(result);
        group.push_back(std::move(s));
    }
    if (result == 3 || result == 4) mon_speech(*m, 0, group);   // sub_729100: a crit / strong crit, its line (S2C 184)
    // 0x002A (30, KR): [u16 victim] + 7×i32 [damage][deathType][result][elementMask][flags][num][guard]
    {
        PacketBuffer d(static_cast<Opcode>(0x002A), is_paula());
        d.write<uint16_t>(target_eid);
        d.write<int32_t>(dmg);                       // [0] damage (client subtracts from local CurHP)
        d.write<int32_t>(0);                         // [1] death type
        d.write<int32_t>(result);                    // [2] result (|0x80 fatal)
        d.write<int32_t>(0);                         // [3] element mask
        d.write<int32_t>(dinfo_flags);               // [4] flags: 0x400 Chi Sword, 0x200 frozen, 1 Harden
        d.write<int32_t>(1);                         // [5] the attacker's kind (sub_715A90: a player)
        d.write<int32_t>(0);                         // [6] guard value
        group.push_back(std::move(d));
    }
    // 0x011E (9): [u16 monster][i32 HP after the hit][u16 gauge=0][u8 multi=0]
    {
        PacketBuffer h(static_cast<Opcode>(0x011E), is_paula());
        h.write<uint16_t>(target_eid);
        h.write<int32_t>(hp);
        h.write<uint16_t>(0);
        h.write<uint8_t>(0);
        group.push_back(std::move(h));
    }
    // 0x002B (3): [u16 monster][u8 deathType=0] — the actual death trigger, inside the block.
    if (killed) {
        PacketBuffer k(static_cast<Opcode>(0x002B), is_paula());
        k.write<uint16_t>(target_eid);
        k.write<uint8_t>(0);
        group.push_back(std::move(k));
    }
    server_.broadcast_block(zone_id_, group, 0);

    // The damage event's kill runs the die handler (sub_7296E0): the owner's TM (sub_891AB0), then the
    // loot (gelder S2C 617, items S2C 12); then the attacker's 30003 gives this swing's EXP (hit or
    // miss — DoAttack posts it for every swing).
    if (killed) { owner_monster_quest(*m); kill_tm_reward(*m); server_.drop_monster_loot(*m, mcx, mcy); }
    gain_attack_exp(*m, dmg, hp_before);
    if ((result & 0x7F) == 0 || (result & 0x7F) == 3 || (result & 0x7F) == 4) {
        crazy_shockwave(*m, mcx, mcy, ap, now);           // a landed blow: Crazy mode's shockwave (4910), then the
        crazy_add(false);                                 //   Crazy Hammer gauge's AtPoint (skill_bear.cpp)
    }

    if (killed) {
        end_attack(true);                   // attacker leaves its attack state (0x0020)
        kill_monster_aftermath(*m, target_eid, mcx, mcy);
        return result;
    }

    // The hit (or miss — the damage event 30002 is posted either way) makes the monster take us
    // as its target. The next attack comes from the follow strategy (state 6 -> 2).
    engage_monster(*m);
    return result;
}

// The engaged monster swings back at the player (report §4.3), as a command block with the SAME
// shape as our melee: 0x001B announce + block{ 0x001F (monster is the attacker, player the victim),
// 0x002A (player victim — the client subtracts this damage from the local HP itself; there is NO
// 0x011E and NO 0x0024 for a player victim) }. On a FATAL hit the player death packets ride inside
// the same block (report §4.6): 0x0019 halt, 0x002B (the death trigger), 0x0020 (player leaves the
// attack state). (The attack the monster's follow strategy triggers in state 6.)
uint8_t GameClientSession::mob_swing(MonsterEntity& mref, std::chrono::steady_clock::time_point now)
{
    MonsterEntity* m = &mref;
    const uint16_t mob_eid  = static_cast<uint16_t>(m->object_id);
    const uint16_t self_eid = static_cast<uint16_t>(object_id_);
    // Retail basic melee (DoAttack sub_41B970 -> sub_5E5700(a5 = 0 for a monster)), monster -> player.
    // The rolls, in this order: the monster's hit — AccFormula 1: sub_5E5680, else sub_5E4390 —, our
    // evade (sub_5E44B0) and block (sub_5E45C0), neither rolled against an AccFormula-1 monster. A
    // protected player (+92 flag 1: after a revive or a zone enter) takes nothing — result 2. A hit
    // neither evaded nor blocked is a crit or a normal hit (the monster's LK / AC); a monster never
    // charges a crit (a5 = 0). Our resistances are 0 (no equipment element values yet), so sub_5E6510
    // changes nothing.
    uint8_t result = 1;
    int32_t mdmg = 0;
    bool hit;
    if (m->acc_formula == 1) {                                           // sub_5E5680
        const int roll = rnd(100);
        int v = 10;                                                      // (a level-0 monster: x87 inf -> 10)
        if (m->level > 0)
            v = static_cast<int>(static_cast<double>(m->ac) -
                                 (static_cast<double>(st_hv_) / 100.0 * 15.0 +
                                  static_cast<double>(level_) / static_cast<double>(m->level) * 15.0));
        if (v <= 10) v = 10;
        hit = roll < v;
    } else {
        hit = rnd(100) < std::min(m->ac, 90);                            // sub_5E4390
    }
    const bool evade = m->acc_formula != 1 && rnd(100) < std::min(st_hv_, 90);
    const bool block = m->acc_formula != 1 && rnd(100) < std::min(st_lk_, 90);
    if (m->cond_test(2)) {
        // DoAttack (sub_423770): a monster under the condition 2 (e.g. the Skunk Pouch's stun 15) announces
        // its swing (0x001B) and rolls, but its blow lands nowhere: no block, no damage event.
        PacketBuffer a(static_cast<Opcode>(0x001B), is_paula());
        int32_t mcx, mcy;
        m->motion.pos(now, mcx, mcy);
        a.write<uint16_t>(mob_eid);
        a.write<uint16_t>(static_cast<uint16_t>(mcx)); a.write<uint16_t>(static_cast<uint16_t>(mcy));
        a.write<uint8_t>(2);
        server_.broadcast_zone(zone_id_, a, 0);
        return 1;
    }
    // Our condition 2048 (ESAFluentWaist, Super Hips) against a monster less than 100 levels above us: no crit
    // roll (sub_5E5700's first branch — a hit neither evaded nor blocked is a normal one).
    const bool no_crit = pcond_test(2048) && static_cast<int32_t>(m->level) - level_ < 100;
    if (protected_) {
        result = 2;
    } else if (hit && !evade) {
        if (block) {
            result = 2;
        } else if (!no_crit && crit_roll(m->lk)) {
            if (strong_roll(m->ac)) { mdmg = melee_strong_damage(m->ap, st_dp_); result = 4; }
            else                    { mdmg = melee_crit_damage(m->ap, st_dp_);   result = 3; }
        } else {
            mdmg = melee_damage(m->ap, st_dp_);
            result = 0;
        }
    }

    // Every attack received (hit or miss) reaches OnDamaged (the 30002 damage event -> Player
    // vtbl+236, sub_73BBC0), which pauses the HP/MP recovery for 6000 ms (sub_73CEF0). A blow that
    // would leave no HP spares us once with the Scroll of the Resolute (ESAPreventExDieScroll 1399): its
    // ES ends (the limiter's vtbl+60, sub_5E28A0) and HP stays at 1 — the event's damage (S2C 0x002A)
    // becomes HP - 1; the swing (0x001F, sent before the event) keeps the blow's.
    regen_pause_ms_ = kRegenPauseMs;
    const int32_t swing_dmg = mdmg;
    // Our ESs' handlers of the damage event (the limiter's vtbl+72 at the head of OnMessage 30002, before the damage
    // is taken): its lists in order, each in its order; each sees the DamageInfo as the ones before left it — the
    // blow's roll made it {damage, death type 0, result (| 0x80 when it would kill, sub_5E5700), 0, 0}.
    //  A Guard (ESAGuard 1309, sub_5B5E10) takes a blow with damage that is no miss / block, of death type 0 (not 1 /
    //   3), without the flags 0x1000 / 0x2000: Count - 1, the damage 0, result 6, DamageInfo +24 = the Count left; at
    //   0 it ends (+224 = 0).
    //  A blessing (1337..1341, counter_on_hit) hits the monster back, whatever the blow; its S2C 923 go into the
    //   swing's block before our 0x2A.
    //  ESADarkBarrier (sub_56D7E0): not result 9 / 10, not yet marked (flag 4), of death type 2 or 0: v = (int)(damage *
    //   ((our MA - 49) * ReflexRatio) * 0.01), at most (int)(damage * 0.5); without the flags 0x1000 / 0x2000 the blow
    //   loses v; v goes back to the attacker as {v, the death type, result, element, flags | 4, +24} after its resist
    //   (sub_5E6760: element 0 = PhysicalR, over 100 it heals) — death type 2 at once, 0 in 200 ms (its tick) —; the
    //   blow is marked (flag 4).
    //  ESAShield (sub_5271D0): with shield left, a blow with damage, not a miss / block / 9, without 0x1000 / 0x2000:
    //   the shield takes it — some left: the damage 0, result 8; else the damage = what went through, it breaks —
    //   and S2C 921 [u16 its id][u32 shield][u32 max] to the area.
    //  ESABind (sub_534B50): once it bound (+308) a damage event ends it.
    //  ESAWhiteKnightDo (a party member's Bodyguard, sub_5631C0): a blow with damage and no flags, its caster there:
    //   half of it (whiteknight_cut).
    int32_t guard_left = -1;
    int32_t in_result  = (mdmg > 0 && mdmg >= hp_) ? (result | 0x80) : result;
    int32_t in_flags   = 0;
    std::vector<PacketBuffer> counter_pk;
    std::vector<CounterHit>   counter_hits;
    std::vector<uint16_t>     es_ended;
    std::vector<size_t>       order;
    for (int list = 0; list < 8; ++list)
        for (size_t k = 0; k < es_.size(); ++k) {
            const int c = es_[k].esclass;
            if (es_[k].limited && ((c == 5 || c == 6) ? 4 : c) == list) order.push_back(k);
        }
    for (size_t k : order) {
        ActiveEs& e = es_[k];
        if (e.kind == EsKind::Guard) {
            if (guard_left >= 0 || e.value <= 0 || mdmg == 0 || result == 1 || result == 2) continue;
            if ((in_flags & 0x1000) || (in_flags & 0x2000)) continue;
            --e.value;
            mdmg      = 0;
            result    = 6;
            in_result = 6;
            guard_left = e.value;
            if (e.value <= 0) { e.value = 0; e.end_ms = 0; }
            LOG_DEBUG("GameClientSession %u: Guard takes mob %u's blow (%d left)", session_id(), mob_eid, guard_left);
            continue;
        }
        if (e.kind == EsKind::Counter) {
            const int32_t in[7] = { mdmg, 0, in_result, 0, in_flags, 0, guard_left >= 0 ? guard_left : 0 };
            counter_on_hit(*m, in, k, counter_pk, counter_hits);
            continue;
        }
        if (e.kind == EsKind::DarkBarrier) {
            if (in_result == 9 || in_result == 10 || (in_flags & 4)) continue;
            int32_t v = static_cast<int32_t>(static_cast<double>(mdmg) *
                                             (static_cast<double>(stat_value(4) - 49) * static_cast<double>(e.ratio)) *
                                             0.009999999776482582);
            const int32_t half = static_cast<int32_t>(static_cast<double>(mdmg) * 0.5);
            if (v > half) v = half;
            if (!(in_flags & 0x1000) && !(in_flags & 0x2000)) mdmg -= v;
            SkillDamage back;
            back.dmg        = v;
            back.death_type = 0;
            back.result     = in_result;
            back.flags      = in_flags | 4;
            back.extra      = guard_left >= 0 ? guard_left : 0;
            skill_resist_hit(*m, 0, back);
            if (back.absorbed > 0) absorb_heal(*m, back.absorbed);   // 30182 to the attacker
            if (back.dmg > 0) {                                      // death type 0: in 200 ms
                e.dinfo[0] = back.dmg;    e.dinfo[1] = back.death_type; e.dinfo[2] = back.result;
                e.dinfo[3] = back.element; e.dinfo[4] = back.flags;     e.dinfo[5] = 0;
                e.dinfo[6] = back.extra;
                e.first        = true;
                e.other        = mob_eid;
                e.interval_acc = 0;
            }
            in_flags |= 4;
            continue;
        }
        if (e.kind == EsKind::HolyShield || e.kind == EsKind::Bunker) {   // (ESABunkerDo sub_5BA760: the same; broken
            if (e.value <= 0) continue;                                    //   -> +228, it ends on its tick)
            if (mdmg == 0 || in_result == 1 || in_result == 2 || in_result == 9) continue;
            if ((in_flags & 0x1000) || (in_flags & 0x2000)) continue;
            e.value -= mdmg;
            if (e.value > 0) {
                mdmg      = 0;
                result    = 8;
                in_result = 8;
            } else {
                mdmg    = std::abs(e.value);
                e.value = 0;
                e.first = true;
            }
            PacketBuffer p(static_cast<Opcode>(0x0399), is_paula());
            p.write<uint16_t>(e.id);
            p.write<int32_t>(e.value);
            p.write<int32_t>(e.count);
            server_.broadcast_zone(zone_id_, p, 0);
            continue;
        }
        if (e.kind == EsKind::WhiteKnightDo) {             // a party Bodyguard's (sub_5631C0): half, no flags
            whiteknight_cut(e, mdmg, in_flags);
            continue;
        }
        if (e.kind == EsKind::Subcutaneous) {
            // ESASubcutaneous (sub_57D6A0): a blow without the flags 0x1000 / 0x2000 / 2, not of death type 4 / 5 (a
            // swing: 0), loses (int)(damage * min(0.5, (int)((our HV * Ratio + 20) / 3) / 100)) (sub_57D800); the flag 2.
            if ((in_flags & 0x1000) || (in_flags & 0x2000) || (in_flags & 2)) continue;
            double v = static_cast<double>(static_cast<int32_t>(
                           (static_cast<double>(stat_value(11)) * static_cast<double>(e.ratio) + 20.0) / 3.0)) / 100.0;
            if (v > 0.5) v = 0.5;
            const int32_t cut = static_cast<int32_t>(static_cast<double>(mdmg) * v);
            if (cut > 0) mdmg -= cut;
            in_flags |= 2;
            continue;
        }
        if (e.kind == EsKind::Bind && e.first) es_ended.push_back(e.id);
    }
    for (uint16_t id : es_ended)
        for (size_t k = 0; k < es_.size(); ++k)
            if (es_[k].id == id) { es_end(k); break; }
    int32_t left = hp_ - mdmg;
    if (left <= 0) {
        left = 0;
        if (es_remove_esaction(1399)) {
            left = 1;
            mdmg = hp_ - 1;
        }
    }
    hp_ = left;
    if (static_cast<double>(hp_max_) * 0.3 >= static_cast<double>(hp_)) pet_speech(6);   // sub_73BDB0: HP <= 30%
    const bool fatal = (hp_ <= 0);
    const bool was_drilling = drill_.active;               // the same 30002 reaches the drill state 3
    if (fatal) hp_ = 0;
    if (fatal) result |= 0x80;

    int32_t mcx, mcy, pcx, pcy;                            // CURRENT positions (sub_4100C0)
    m->motion.pos(now, mcx, mcy);
    cur_pos(now, pcx, pcy);
    const uint16_t mx = static_cast<uint16_t>(mcx), my = static_cast<uint16_t>(mcy);
    const uint16_t px = static_cast<uint16_t>(pcx), py = static_cast<uint16_t>(pcy);

    LOG_DEBUG("GameClientSession %u: mob %u hits player d=%d for %d -> hp=%d/%d%s",
             session_id(), mob_eid, dist_i(mcx, mcy, pcx, pcy), mdmg, hp_, hp_max_,
             fatal ? " [PLAYER DEAD]" : "");

    // 0x001B (7) — the monster's swing announcement (block 0, area). [u16 id][u16 x][u16 y][u8 2]
    {
        PacketBuffer a(static_cast<Opcode>(0x001B), is_paula());
        a.write<uint16_t>(mob_eid);
        a.write<uint16_t>(mx); a.write<uint16_t>(my);
        a.write<uint8_t>(2);
        server_.broadcast_zone(zone_id_, a, 0);
    }

    std::vector<PacketBuffer> group;
    // 0x001F (17): [u16 mob][u16 kind=1][u16 player][u16 mx][u16 my][u16 px][u16 py][i16 dmg][u8 result]
    {
        PacketBuffer s(static_cast<Opcode>(0x001F), is_paula());
        s.write<uint16_t>(mob_eid);
        s.write<uint16_t>(1);
        s.write<uint16_t>(self_eid);
        s.write<uint16_t>(mx); s.write<uint16_t>(my);
        s.write<uint16_t>(px); s.write<uint16_t>(py);
        s.write<int16_t>(static_cast<int16_t>(swing_dmg));
        s.write<uint8_t>(result);
        group.push_back(std::move(s));
    }
    for (PacketBuffer& p : counter_pk) group.push_back(std::move(p));   // the blessings' S2C 923 (+ 0x2B)
    // 0x002A (30): [u16 player] + 7×i32 — the player is the victim here.
    {
        PacketBuffer d(static_cast<Opcode>(0x002A), is_paula());
        d.write<uint16_t>(self_eid);
        d.write<int32_t>(mdmg);                      // [0] damage (client subtracts from local CurHP)
        d.write<int32_t>(0);                         // [1] death type
        d.write<int32_t>(result);                    // [2] result (|0x80 fatal; 6 guarded)
        d.write<int32_t>(0); d.write<int32_t>(0); d.write<int32_t>(0);
        d.write<int32_t>(guard_left >= 0 ? guard_left : 0);   // [6] (+24: a Guard's Count left)
        group.push_back(std::move(d));
    }
    if (fatal) player_die_packets(group, pcx, pcy);   // inside the fatal swing block (report §4.6)
    server_.broadcast_block(zone_id_, group, 0);
    es_on_damaged();                       // the ESs' hooks of the damage event (ESASelfCureDo ends)
    for (const CounterHit& h : counter_hits)   // a blessing's kill: the monster's die handler (no EXP: no 30003)
        skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, true, now, 1);

    if (fatal) {
        player_on_die(pcx, pcy);
        return result;
    }
    if (was_drilling) drill_interrupted(); // state 3, 30002: the dig ends (stop 10) unless PSAbility 6
    wh_hit();                              // state 10, 30002: out of the warehouse state
    return result;
}

// Our death's packets, at the end of the fatal damage event's block (report §4.6; Player vtbl+248 with the blow's
// death type). (0x0026 exp-loss is self-only and deferred — it is not needed for the death itself.)
void GameClientSession::player_die_packets(std::vector<PacketBuffer>& group, int32_t pcx, int32_t pcy, uint8_t death_type)
{
    const uint16_t self_eid = static_cast<uint16_t>(object_id_);
    // 0x0019 (7): [u16 player][u16 x][u16 y][u8 moveType] — halt (0 written as 1).
    {
        PacketBuffer hl(static_cast<Opcode>(0x0019), is_paula());
        hl.write<uint16_t>(self_eid);
        hl.write<uint16_t>(static_cast<uint16_t>(pcx)); hl.write<uint16_t>(static_cast<uint16_t>(pcy));
        hl.write<uint8_t>(1);
        group.push_back(std::move(hl));
    }
    // 0x002B (3): [u16 player][u8 death type] — THE death trigger (client enters the dead state only here).
    {
        PacketBuffer k(static_cast<Opcode>(0x002B), is_paula());
        k.write<uint16_t>(self_eid);
        k.write<uint8_t>(death_type);
        group.push_back(std::move(k));
    }
    // 0x0020 (2): [u16 player] — the player leaves its attack state.
    {
        PacketBuffer lv(static_cast<Opcode>(0x0020), is_paula());
        lv.write<uint16_t>(self_eid);
        group.push_back(std::move(lv));
    }
}

// Player::OnDie (sub_73BFA0) after the fatal damage event.
void GameClientSession::player_on_die(int32_t pcx, int32_t pcy)
{
    dead_ = true;
    hp_ = 0;                           // (+444 = 0)
    server_.party().dead(char_uid_);   // the party: out of the share (Party::OnNotifyDead), our HP 0 to the mates
    rest_state_ = 0;                   // the dead state 2 replaces sitting / resting
    shop_ = 0;                         // and the shop state
    exchange_ = false;                 // and the exchange shop state
    if (trade_ && !trade_sync_) { trade_reset(); trade_leave(); }   // and the trade's state 6 (trade.cpp)
    crazy_end(true);                   // and the Bear's Crazy mode (skill_bear.cpp; the gauge stays)
    wh_leave_state();                  // and the warehouse state 10
    compound_leave();                  // and the compound state 13
    refine_ui_leave();                 // and the refine / growth / reset states 30..32
    skill_shop_leave();                // and the skill master state 25
    if (disguised_) {                  // OnDie: a disguise ends (sub_7695B0) + S2C 0x01E4 [19][0][0]
        if ((state_type_ & 16) && shown_model_ != type_) disguise_end(0);
        send_system_msg(19, 0, 0);
    }
    pet_speech(3);                     // our pet's line (sub_738F50(3): a running pause ends first)
    // Unless the no-penalty flag (+4684), 2% of the EXP (int) is the loss, kept with the TM loss for a beginner's
    // restart (+4772 / +4776, sub_76DE10); the EXP drops by it times the ExpPenalty (+1244: 1.0 without an
    // Item_ExpParam item in slot 4), at least 0 (sub_76DE40) -> S2C 0x26 [u32 EXP] (sub_5F4DA0; client: avatar
    // +912). The TM loss needs that item (2% of the TM EXP times its TmxpPenalty, +1252 = 0.0 otherwise).
    lost_exp_ = 2 * exp_ / 100;
    lost_tm_  = 0;
    exp_ -= static_cast<int32_t>(static_cast<double>(lost_exp_) * 1.0f);
    if (exp_ < 0) exp_ = 0;
    {
        PacketBuffer e(static_cast<Opcode>(0x0026), is_paula());
        e.write<uint32_t>(static_cast<uint32_t>(exp_));
        send(e);
    }
    death_unequip();                   // then a unique item of slots 0..2 may come off (equip.cpp)
    es_end_death_lists();              // the ESLimiter lists 0, 1 and 7 end (es.cpp)
    server_.release_owned(object_id_); // sub_717DB0: the monsters we own let us go (S2C 366 each)
    cdm_on_death();                    // the Crazy Drill Mode ends (sub_7B0230, drill.cpp)
    monq_on_death();                   // a running monster quest stops (monster_quest.cpp)
    attacking_ = false; pf_.state = 0; // our follow stops (the death block carries our 0x0020)
    su_ = SkillUse{};                  // a skill being selected / cast is dropped
    pick_ = Pickup{};                  // and a pick-up walk
    drill_ = Drill{};                  // and the drill state 3
    motion_.rest(pcx, pcy);            // 0x0019 halts us where we stand
    pos_x_ = pcx; pos_y_ = pcy;
    end_all_mob_attacks(true);         // our hate entries die with us: each monster leaves its
                                       //   battle state (0x0020 mob)
}

// A monster we killed (melee or skill): the corpse stays where it died, the monster's counter-attack
// stops (no 0x0020 — it is dead), and the corpse is removed ~2000 ms later with 0x000F [u16 id][u8 0]
// (report §2.6).
void GameClientSession::kill_monster_aftermath(MonsterEntity& m, uint16_t eid, int32_t x, int32_t y)
{
    m.motion.rest(x, y);
    m.pos_x = x; m.pos_y = y;
    if (engaged_with(eid))
        end_mob_attack(eid, false);
    const int16_t zone = zone_id_;
    auto self = shared_from_this();
    schedule_delayed(std::chrono::milliseconds(2000), [this, self, eid, zone]() {
        PacketBuffer rm(static_cast<Opcode>(0x000F), is_paula());
        rm.write<uint16_t>(eid);
        rm.write<uint8_t>(0);
        server_.broadcast_zone(zone, rm, 0);
        if (const MonsterEntity* gone = server_.entity_mgr().get_monster(eid))
            server_.monster_removed(*gone);                  // its map's regeneration counts it out
        server_.entity_mgr().despawn(eid);
    });
}

void GameClientSession::end_mob_attack(uint16_t eid, bool send_leave)
{
    auto it = std::find_if(mobs_.begin(), mobs_.end(), [&](const MobEngage& e) { return e.eid == eid; });
    if (it == mobs_.end()) return;
    mobs_.erase(it);
    MonsterEntity* m = server_.entity_mgr().get_monster(eid);
    if (m && m->target_oid == object_id_) {                   // idle again (monster_ai.cpp): state 0
        m->target_oid    = 0;                                  // restarts its walk (sub_433120: the
        m->walk_state    = 1;                                  // timer = the min delay, state 1)
        m->walk_timer_ms = 3000;
    }
    if (send_leave && m && m->alive) {
        // S2C 0x0020 (2): [u16 id] — the monster leaves its battle state (area).
        PacketBuffer lv(static_cast<Opcode>(0x0020), is_paula());
        lv.write<uint16_t>(eid);
        server_.broadcast_zone(zone_id_, lv, 0);
    }
}

void GameClientSession::end_all_mob_attacks(bool send_leave)
{
    while (!mobs_.empty()) end_mob_attack(mobs_.back().eid, send_leave);
}

// Death-screen restart, as the retail Player state machine (sub_73D2A0, not decompiled — read from
// its disassembly). The client sends both only from its death screen (RestartUI sub_A46740; the
// menu's "Continue" while its actor is dead, sub_8DD480).
//   C2S 0x0016 "Go to Town" (case 22): at/below BeginnerRebirthInfo.Level the death's loss comes back
//     first (sub_764100); HP = MaxHP*30/100 (MP untouched); Zone::RestartPlayer(the player's return
//     zone, +1656) = sub_7FDE60 -> sub_5F7B70: S2C 0x0017 [u16 zone][pos 0,0][u32 HP] to the player +
//     S2C 0x0015 [u16 id] to the area, and a field change through Master (S2C 154 -> client
//     reconnects with 156 -> EnterZone); then the loss is gone (sub_76CF20) and the state is Normal.
//     (Not here: the +328 flag 4 that ignores it and the conditions 2/3/8 it ends — S2C 183/318/816,
//     sub_73C9C0 / sub_73CAF0 / sub_73CCD0 — no such conditions exist here yet.)
//   C2S 0x02F7 "Resurrect" (case 759): in place, NO field change. At/below BeginnerRebirthInfo.Level
//     (libconfig: 20) it is free: the revive with its ratios (sub_764260, all 1.0) -> S2C 0x02F8
//     [u16 id][u16 0] to the area (sub_601C90); a refused revive -> S2C 0x02F9 (sub_601D20; client
//     "msg_not_proper_object"). Above it the bag's LAST item of Effect 29 (ItemParam +84) is used
//     (Player::UseItem vtbl+144 -> UseItemHelper::Use case 29, item_use.cpp); none, or a failed use
//     -> S2C 0x005A (sub_5F6730).
static constexpr float kBeginnerRestoreHpRatio   = 1.0f;  // libconfig BeginnerRebirthInfo.RestoreHpRatio
static constexpr float kBeginnerRestoreMpRatio   = 1.0f;  // libconfig BeginnerRebirthInfo.RestoreMpRatio
static constexpr float kBeginnerRestoreExpRatio  = 1.0f;  // libconfig BeginnerRebirthInfo.RestoreExpRatio
static constexpr float kBeginnerRestoreTmxpRatio = 1.0f;  // libconfig BeginnerRebirthInfo.RestoreTmxpRatio
static constexpr int   kTownRestartHpPercent  = 30;       // case 22: +444 = +596 * 30 / 100
static constexpr int   kRebirthEffect         = 29;       // ItemParam Effect (+84) of a resurrection item
static constexpr int   kReviveDoneMs          = 1500;     // sub_764260: event 30200 in 1500 ms
static constexpr int16_t kReviveProtectMs     = 5000;     // sub_764460: sub_73CF80(5000, 0)

void GameClientSession::handle_restart(Opcode op)
{
    const bool town = static_cast<uint16_t>(op) == 0x0016;
    const uint16_t self_eid = static_cast<uint16_t>(object_id_);

    // Resurrect: a player who is not dead gets the retail answers (the revive refuses: 0x02F9 / 0x005A).
    if (!town) {
        if (level_ <= kBeginnerResurrectLevel) {
            if (!revive(kBeginnerRestoreHpRatio, kBeginnerRestoreMpRatio, kBeginnerRestoreExpRatio,
                        kBeginnerRestoreTmxpRatio)) {
                PacketBuffer no(static_cast<Opcode>(0x02F9), is_paula());   // __fputchar_113
                send(no);
                return;
            }
            PacketBuffer r(static_cast<Opcode>(0x02F8), is_paula());        // sub_601C90(us, 0)
            r.write<uint16_t>(self_eid);
            r.write<uint16_t>(0);
            server_.broadcast_zone(zone_id_, r, 0);
            server_.char_store().SaveState(char_uid_, zone_id_, pos_x_, pos_y_, money_, level_, exp_, hp_, mp_);
            LOG_INFO("GameClientSession %u: Resurrect in place (beginner) zone=%d (%d,%d) hp=%d/%d mp=%d/%d",
                     session_id(), zone_id_, pos_x_, pos_y_, hp_, hp_max_, mp_, mp_max_);
            return;
        }
        // The bag's last item of Effect 29 (the bag iterator, +1716), used as Player::UseItem.
        BagItem* found = nullptr;
        const ItemDef* found_def = nullptr;
        for (BagItem& b : bag_) {
            const ItemDef* d = server_.item_db().get(b.item_id);
            if (d && d->effect == kRebirthEffect) { found = &b; found_def = d; }
        }
        if (!found || !use_item(*found, *found_def)) {
            if (!found)
                LOG_INFO("GameClientSession %u: Resurrect refused — level %d > %d and no resurrection item",
                         session_id(), level_, kBeginnerResurrectLevel);
            PacketBuffer p(static_cast<Opcode>(0x005A), is_paula());          // sub_5F6730
            send(p);
        }
        return;
    }

    // Go to Town — kept to the dead (retail case 22 has no such check, but its client sends it only
    // from the death screen; alive it would be a free trip to town).
    if (!dead_) return;
    head_notice_end();                                     // case 22: the conditions 2 / 3 / 8 end (megaphone.cpp)
    // A beginner (level <= BeginnerRebirthInfo.Level) gets the death's loss back first.
    if (level_ <= kBeginnerResurrectLevel)
        restore_lost(kBeginnerRestoreExpRatio, kBeginnerRestoreTmxpRatio);
    lost_exp_ = 0;                             // sub_76CF20
    lost_tm_  = 0;

    dead_ = false;
    reviving_ = false;                         // (a revive still waiting for its 30200 is over)
    attacking_ = false;      pf_.state = 0;    // make sure no stale combat survives the restart
    end_all_mob_attacks(false);                // (the world tick keeps running: recovery resumes)

    {
        hp_ = hp_max_ * kTownRestartHpPercent / 100;
        // Destination = the return town (+1656) — or, with the ProjectPAULA option
        // `restart_town: zone`, the town of the zone we died in (maps_db Town; 0 -> return town).
        // The request carries pos (0,0) with changeZoneType 1, so the destination's sub_810940 finds
        // it invalid and uses the zone default arrival (sub_8108F0: random type-2 map point, else
        // random type-1, else (500,400)).
        int16_t town_zone = home_;
        if (server_.restart_town_of_zone()) {
            const MapDef* here = server_.maps_db().get(zone_id_);
            if (here && here->town > 0) town_zone = static_cast<int16_t>(here->town);
        }
        int32_t ax = 0, ay = 0;
        server_.maps_db().default_arrival(town_zone, ax, ay);
        PacketBuffer r(static_cast<Opcode>(0x0017), is_paula());
        r.write<uint16_t>(static_cast<uint16_t>(town_zone));
        r.write<uint16_t>(0);                  // pos (0,0): the destination resolves the arrival point
        r.write<uint16_t>(0);
        r.write<uint32_t>(static_cast<uint32_t>(hp_));
        send(r);
        PacketBuffer g(static_cast<Opcode>(0x0015), is_paula());
        g.write<uint16_t>(self_eid);
        server_.broadcast_zone(zone_id_, g, 0);
        LOG_INFO("GameClientSession %u: Go to Town -> return town %d (%d,%d) hp=%d/%d (30%%)",
                 session_id(), town_zone, ax, ay, hp_, hp_max_);
        begin_field_change(town_zone, ax, ay, 1);   // 154 -> client 156 -> EnterZone
    }
}

// Player revive (sub_764260), in place: only a dead player (state 2) whose revive is not already
// running (+4768, set here). The player's states are reset (sub_7FFC50 / sub_764620: the attack, the
// target, the conditions — their Master/DB notices are not modelled); HP = (int)(MaxHP * hp), MP =
// max(MP, (int)(MaxMP * mp)); the death's loss comes back by the ratios (sub_764100) and is gone
// (sub_76CF20). The player stays dead (state 2, +92 flag 4) until the event 30200 1500 ms later
// (revive_done). (Not here: the pet of equipment slot 8 is summoned again — sub_6FCAE0, MyCamp — and
// a guardian zone's Soulshade, sub_76A5D0: neither exists here yet.)
bool GameClientSession::revive(float hp, float mp, float exp, float tmxp)
{
    if (!dead_ || reviving_) return false;
    reviving_ = true;
    attacking_ = false;  pf_.state = 0;
    su_   = SkillUse{};
    pick_ = Pickup{};
    end_all_mob_attacks(false);
    hp_ = static_cast<int32_t>(static_cast<double>(hp_max_) * hp);
    const int32_t m = static_cast<int32_t>(static_cast<double>(mp_max_) * mp);
    if (mp_ < m) mp_ = m;
    restore_lost(exp, tmxp);
    lost_exp_ = 0;
    lost_tm_  = 0;
    if (third_eye_) third_eye_ = false;                       // +1628 off (sub_426950 / sub_719920)
    const int16_t zone = zone_id_;
    auto self = shared_from_this();
    schedule_delayed(std::chrono::milliseconds(kReviveDoneMs), [this, self, zone]() {
        if (zone_id_ != zone || state_ != GameClientSessionState::InWorld) return;
        revive_done();
    });
    return true;
}

// sub_764100: the death's loss comes back by the ratios — EXP + (int)(lost * exp) through AddExp
// (vtbl+168 with a4 = 1: the raw amount) -> S2C 0x0026 [u32 EXP] (sub_5F4DA0); TM EXP + (int)(lost TM *
// tmxp) through AddTMExp (sub_716C30, raw) -> S2C 0x011D [u8 FType][u32 TM EXP] (sub_5ED650). Both are
// sent even when nothing comes back.
void GameClientSession::restore_lost(float exp, float tmxp)
{
    const int32_t tm = static_cast<int32_t>(static_cast<double>(lost_tm_) * tmxp);
    add_exp(static_cast<int32_t>(static_cast<double>(lost_exp_) * exp));
    PacketBuffer e(static_cast<Opcode>(0x0026), is_paula());
    e.write<uint32_t>(static_cast<uint32_t>(exp_));
    send(e);
    add_tm_exp(tm);
    PacketBuffer t(static_cast<Opcode>(0x011D), is_paula());
    t.write<uint8_t>(f_type_);
    t.write<uint32_t>(static_cast<uint32_t>(tm_exp_));
    send(t);
}

// The event 30200 of a revive (sub_764460): the restart point (+3128) and the +4780 summon state are
// cleared, the state flags (+92) go to 0 and the state machine to Normal; our appear (S2C 8,
// sub_5F6F40) to the area, us included — the others see us standing again; S2C 0x0024 [u32 HP] /
// 0x0025 [u32 MP] (sub_5F4670 / sub_5F49B0) to us; 5000 ms of protection (sub_73CF80(5000, 0), no
// S2C 798); S2C 0x02FB (__fputchar_114 — the client ends its revive: sub_49DA80, avatar +0x5C = 0,
// +0x6790 = 0, sub_6ADBC0); +4768 = 0.
void GameClientSession::revive_done()
{
    if (!reviving_) return;
    dead_ = false;
    server_.party().revived(char_uid_);        // the party's MEMBER REBIRTH (sub_4F4CB0)
    PacketBuffer ap = build_appear(0x0008);
    server_.broadcast_zone(zone_id_, ap, 0);
    PacketBuffer h(static_cast<Opcode>(0x0024), is_paula());
    h.write<uint32_t>(static_cast<uint32_t>(hp_));
    send(h);
    PacketBuffer m(static_cast<Opcode>(0x0025), is_paula());
    m.write<uint32_t>(static_cast<uint32_t>(mp_));
    send(m);
    protect(kReviveProtectMs);
    PacketBuffer done(static_cast<Opcode>(0x02FB), is_paula());
    send(done);
    reviving_ = false;
    LOG_INFO("GameClientSession %u: revived zone=%d (%d,%d) hp=%d/%d mp=%d/%d exp=%d", session_id(), zone_id_,
             pos_x_, pos_y_, hp_, hp_max_, mp_, mp_max_, exp_);
}

// C2S 0x0040 [u8 action] (Player case 64, a message of every state) -> sub_75C850:
//   0  stand up: unless dead -> state 0;
//   2  sit: CanSit (sub_75C780: the normal form (+1060 flag 1 — a transformed player gets S2C 0x01E4
//      [13] / [20], none exist here), in state 0 or 15, standing still or on a cell of attribute 4 —
//      that cell case is not modelled) -> state 8; refused: nothing at all;
//   13 rest: the normal form -> state 15 (the client sends it while sitting, sub_6AFF70);
//   any other value is only shown (an emotion).
// What happens is shown to the area, us included: S2C 0x0040 [u16 id][u8 action] (sub_5F44B0; client
// sub_6BEC50 case 0x40 -> the actor's vtbl+128). The client asks for it only when its avatar stands
// still and out of attack mode (sub_6AFF90). Entering 0 / 8 / 15 sets the recovery rates (regen_tick).
void GameClientSession::handle_action(PacketBuffer& pkt)
{
    try {
        const uint8_t a = pkt.read<uint8_t>();
        if (shop_) return;                                     // the shop state 5 swallows it (case 64)
        if (wh_state_) return;                                 // so does the warehouse state 10
        if (bank_state_) return;                               // the bank state 11
        if (sshop_) return;                                    // and the skill master state 25
        if (refine_ui_) return;                                // and the refine / growth / reset states
        if (a == 0) {
            if (dead_) return;
            rest_state_ = 0;
            drill_ = Drill{};                                  // (state 0 — also out of the drill state 3
            compound_leave();                                  //  and the compound state 13)
        } else if (a == 2) {
            const bool normal = !dead_ && !talk_npc_ && !su_.active && !pick_.active && !drill_.active &&
                                !compound_;                    // state 0 ...
            if (!(normal && (rest_state_ == 0 || rest_state_ == 15))) return;          // ... or 15
            if (motion_.moving(std::chrono::steady_clock::now())) return;
            pet_speech(10);                                    // our pet's line, then the state 8
            rest_state_ = 8;
        } else if (a == 13) {
            // (Retail checks only the form here; a dead player would leave the dead state — refused.)
            if (dead_) return;
            pet_speech(11);                                    // our pet's line, then the state 15
            rest_state_ = 15;
            drill_ = Drill{};
            compound_leave();
        }
        PacketBuffer p(static_cast<Opcode>(0x0040), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<uint8_t>(a);
        server_.broadcast_zone(zone_id_, p, 0);
        LOG_DEBUG("GameClientSession %u: action %u -> rest state %d", session_id(), a, rest_state_);
    } catch (...) {}
}

// sub_73CF80(ms, 0): the protection (+92 flag 1) for `ms` (+3252) — no S2C 798 (its third argument
// is 0 everywhere we call it).
void GameClientSession::protect(int16_t ms)
{
    protect_ms_ = ms;
    protected_  = true;
}

// sub_73CFC0, in the Player tick while alive (state != 2): the protection runs out.
void GameClientSession::protect_tick(int elapsed_ms)
{
    if (protect_ms_ > 0) {
        protect_ms_ = static_cast<int16_t>(protect_ms_ - elapsed_ms);
        if (protect_ms_ <= 0) {
            protect_ms_ = 0;
            protected_  = false;
        }
    }
}

// Derive the character's effective combat stats from level + the P/M/S/C growth graph, exactly as
// the retail GameServer (sub_7164C0 + GetChrStatusValue): for each of the 12 canonical stats,
// AccPts = growthWeight(group) * (level-1); graphLevel = AccPts/4 + 1; Stat = gulim13[graphLevel][i]
// (equip/buff/base terms are 0 on a plain character). MaxHP/MaxMP come straight from the table.
// Groups: Power->{AP,AC,DX}, Magic->{MaxMP,MA,MD}, Sense->{MaxWT,DA,LK}, Charm->{MaxHP,DP,HV}.
void GameClientSession::recompute_stats(bool clamp)
{
    const StatTable& T = server_.stat_table();
    if (!T.loaded()) return;                       // no table -> keep the fallback stats

    // Final stat (sub_716680) = base (+704: gulim13[graph-level], graph-level = accpts/4 + 1) + the
    // equipped items' bonuses (+608, sub_718750) + the buffs (+656: ResChain 9 sub_716820 on
    // (float)(base + items), es.cpp); the other terms (vtbl+260, +1424, the ratio sub_719A30) are 0.
    int32_t bonus[ST_COUNT];
    equip_bonus(bonus);
    if (base_fixed_)                               // the points changed: sub_7164C0 wrote the base from the table,
        for (int k = 0; k < ST_COUNT; ++k)         //   then a mutation's vtbl+108 makes it again (sub_5E1A10)
            if (base_fixed_pts_[k] != accpts_[k]) { base_fixed_ = false; mutation_rebase(); break; }
    int stat[ST_COUNT];
    for (int i = 0; i < ST_COUNT; ++i) {
        const int32_t base_item = base_stat(i) + bonus[i];
        buff_[i] = es_stat_bonus(i, static_cast<float>(base_item));
        stat[i]  = base_item + buff_[i];
        stat_[i] = stat[i];
    }

    st_ap_ = stat[ST_AP];  st_ac_ = stat[ST_AC];  st_dx_ = stat[ST_DX];
    st_da_ = stat[ST_DA];  st_lk_ = stat[ST_LK];  st_dp_ = stat[ST_DP];  st_hv_ = stat[ST_HV];
    st_maxwt_ = stat[ST_MAXWT];                    // Player +584 (this[146]) = stat 6
    if (stat[ST_MAXHP] > 0) hp_max_ = stat[ST_MAXHP];
    if (stat[ST_MAXMP] > 0) mp_max_ = stat[ST_MAXMP];
    if (clamp) {                                   // sub_76A4F0(a2 = 1): HP / MP within the new maxima
        if (hp_ > hp_max_) hp_ = hp_max_;
        if (mp_ > mp_max_) mp_ = mp_max_;
    }

    LOG_DEBUG("GameClientSession %u: stats lvl=%d -> AP=%d AC=%d DX=%d DA=%d LK=%d DP=%d HV=%d HP=%d MP=%d "
             "(unspent=%d)", session_id(), level_, st_ap_, st_ac_, st_dx_, st_da_, st_lk_, st_dp_, st_hv_,
             hp_max_, mp_max_, attr_pts_);
}

// Seed the accumulated points from the P/M/S/C growth graph (× level-1) — the auto-distribution a
// character has with no manual re-allocation — and set the unspent pool (34/level total, 30 auto +
// 4 free). Then recompute the effective stats. Called on load and on @blvl.
void GameClientSession::init_accpts_from_growth(bool clamp)
{
    const int lvlm1 = (level_ > 1) ? (level_ - 1) : 0;
    const int gw[ST_COUNT] = {
        pts_p_, pts_p_, pts_p_,     // AP, AC, DX        (Power)
        pts_m_, pts_m_, pts_m_,     // MaxMP, MA, MD     (Magic)
        pts_s_, pts_s_, pts_s_,     // MaxWT, DA, LK     (Sense)
        pts_c_, pts_c_, pts_c_,     // MaxHP, DP, HV     (Charm)
    };
    for (int i = 0; i < ST_COUNT; ++i) accpts_[i] = gw[i] * lvlm1;
    attr_pts_ = static_cast<int16_t>(4 * lvlm1);   // unspent = 34/level - 30 auto = 4/level
    recompute_stats(clamp);
}

// C2S 0x006C — stat distribution "accept" (Player case 108): body = 12 × int16 points-to-add (canonical
// order). The retail applier sub_73C840 (each delta >= 0, sum <= unspent) spends them from the unspent
// pool into the accumulated points and recomputes; the answer is S2C 0x006D [u16 unspent][12 × u16
// accumulated points] (sub_5F4FF0), a refusal S2C 0x006E, empty (sub_5F3B40); then the DB (3613).
// VERIFIED in the client: sub_6F0960 case 109 stores the u16 (avatar +712) and the 24 bytes (+664),
// recomputes (vtbl+288) and refreshes the stat window; case 110 resets it. (S2C 0x0275 — what this used
// to send — is the status reset's answer: the KR client says "ResetStatusOk" on it.)
void GameClientSession::handle_stat_alloc(PacketBuffer& pkt)
{
    auto refuse = [&]() {
        PacketBuffer p(static_cast<Opcode>(0x006E), is_paula());
        send(p);
    };
    try {
        int16_t delta[ST_COUNT];
        long sum = 0;
        bool bad = false;
        for (int i = 0; i < ST_COUNT; ++i) {
            delta[i] = pkt.read<int16_t>();
            if (delta[i] < 0) bad = true;          // can't un-spend
            sum += delta[i];
        }
        if (bad || sum <= 0 || sum > attr_pts_) {
            LOG_INFO("GameClientSession %u: stat alloc rejected (sum %ld, unspent %d)", session_id(), sum, attr_pts_);
            refuse();
            return;
        }
        attr_pts_ = static_cast<int16_t>(attr_pts_ - sum);
        for (int i = 0; i < ST_COUNT; ++i) accpts_[i] += delta[i];
        recompute_stats();
        save_stat_points();
        send_points();
        LOG_DEBUG("GameClientSession %u: stat alloc applied (spent %ld, unspent now %d) -> AP=%d DP=%d HP=%d",
                 session_id(), sum, attr_pts_, st_ap_, st_dp_, hp_max_);
    } catch (...) {}
}

// The 33-byte character stat block — byte-for-byte the retail serializer sub_75CB40 (GameServer)
// and what the client reads in Apply_33byte_StatsBlock (sub_6ABC20):
//   +0..+22 12×u16 accumulated points (canonical AP AC DX MaxMP MA MD MaxWT DA LK MaxHP DP HV)
//   +24 u16 unspent pool   +26 u16 TM points   +28 u16 TM level   +30 u16 base level   +32 u8 (char+3016)
// The client handlers read +0..+24, +28, +30; +26 and +32 are not read there (0 until tracked).
void GameClientSession::write_stat_block(PacketBuffer& p) const
{
    for (int i = 0; i < ST_COUNT; ++i) p.write<uint16_t>(static_cast<uint16_t>(accpts_[i]));
    p.write<uint16_t>(static_cast<uint16_t>(attr_pts_));
    p.write<uint16_t>(static_cast<uint16_t>(tm_points_));  // TM points (+4592)
    p.write<uint16_t>(static_cast<uint16_t>(tm_level_));
    p.write<uint16_t>(static_cast<uint16_t>(level_));
    p.write<uint8_t>(0);                                    // char+3016 (not read by these client handlers)
}

// S2C 0x0275 (629) — stat-block refresh. Retail: sub_600710, the answer of a status reset (Point Back,
// item_use.cpp); client: case 0x275 of the local-player handler sub_6B4E00 → Apply_33byte_StatsBlock + the
// My-View / UI refresh + the line "ResetStatusOk". Used here by the GM level command too.
void GameClientSession::send_stat_refresh()
{
    PacketBuffer p(static_cast<Opcode>(0x0275), is_paula());
    write_stat_block(p);
    send(p);
}

// S2C 0x0027 (39) — the game's own level-up. Retail builder sub_5F4EF0: [block][u32 curHP][u32 curMP]
// [u32 exp] (45 B on the 2012 server); the KR client (case 0x27 in sub_6B4E00) reads one more u8 flag
// (46 B) and plays the "level up" effect + "etc_levelup" sound + LevelUpPtsStr (4 × levels gained).
void GameClientSession::send_level_up()
{
    PacketBuffer p(static_cast<Opcode>(0x0027), is_paula());
    write_stat_block(p);
    p.write<uint32_t>(static_cast<uint32_t>(hp_));
    p.write<uint32_t>(static_cast<uint32_t>(mp_));
    p.write<uint32_t>(static_cast<uint32_t>(exp_));
    p.write<uint8_t>(0);                                    // KR-client extra flag (0 = normal branch)
    send(p);
}

void GameClientSession::save_stat_points()
{
    if (!char_uid_) return;
    server_.char_store().SaveStatPoints(char_uid_, accpts_, attr_pts_, tm_level_, tm_exp_, tm_points_);
}

void GameClientSession::handle_heartbeat(PacketBuffer& /*pkt*/, Opcode /*op*/)
{
    PacketBuffer ack(Opcode::HEARTBEAT_ACK, is_paula());
    send(ack);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

void GameClientSession::enter_zone(int16_t zone_id, int32_t x, int32_t y)
{
    if (!char_uid_) return;                 // never before a character is loaded (its stable id: spawn_player)
    if (object_id_)
        server_.entity_mgr().despawn(object_id_);

    zone_id_ = zone_id;
    pos_x_ = x;
    pos_y_ = y;
    motion_.rest(x, y);

    PlayerEntity ent{};
    ent.type       = EntityType::Player;
    ent.zone       = zone_id;
    ent.pos_x      = x;
    ent.pos_y      = y;
    ent.char_uid   = char_uid_;
    ent.account_id = account_id_;
    ent.session_id = session_id();

    object_id_ = server_.entity_mgr().spawn_player(ent);
}

void GameClientSession::begin_field_change(int16_t zone, int32_t x, int32_t y, uint8_t change_type)
{
    // Field change via RELOCATE (154) with the destination + startPos. EMPIRICAL (game_live.log):
    // the client does NOT change map in place — it TEARS DOWN this connection and opens a FRESH
    // TCP connection to the dest addr/port in the 154, then sends C2S 156 (CMD_ENTER_ZONE) as the
    // first packet of that new connection, re-presenting its chrUid token. So the warp is really a
    // reconnect+re-login (matching the real server's case 156 -> re-create player -> spawn).
    //
    // Because the reconnected session starts with NO state, it must load the char fresh from the
    // DB. Persist the destination zone/pos NOW so the new session reads the right landing spot.
    // CRITICAL: also move THIS session's own zone/pos to the destination, otherwise when this
    // (abandoned) connection finally closes, on_close -> save_position() writes the STALE origin
    // position back over the destination, and the reconnected session loads the wrong zone
    // (observed in game_live.log: warp to zone2/250,250 but session 2 loaded zone1/2980,2684).
    // Client::PrepareLeavingZone (sub_40B800): the ESLimiter's effect states go along (es.cpp).
    trade_close();                      // a trade in its DB step settles first: SaveState below writes its gelders
    relocated_ = true;                  // (our close is the old connection's now: on_close)
    es_carry_out(zone);
    crazy_carry_out();                  // and the Bear's Crazy Hammer gauge / mode (skill_bear.cpp)
    mega_on_ = false;                   // (the Mic's head notice is the old Player's: not carried)
    mega_text_.clear();
    pet_destroy(false);                 // our pet leaves the old zone with us (made again on the 156)
    server_.disguise_put(char_uid_, disguised_ ? shown_model_ : type_);   // (PrepareLeavingZone: +6208 / +436)
    server_.party().leave_zone(char_uid_, zone);                         // the party sees us move (S2C 7035)
    zone_id_ = zone; pos_x_ = x; pos_y_ = y;
    motion_.rest(x, y);
    // The effect states' ids go back now (ES-02): es_carry_out copied what goes along (the 156 session makes
    // them again with new ids) and this connection is done with them — kept, they were lost with it. es_end_all
    // sends nothing; cast_es_end_all ends our skills' objects on the old zone's monsters (S2C 0x118 to that
    // zone), as our tick did anyway once zone_id_ is the destination (cast_es_tick: the target is not here).
    es_end_all();
    cast_es_end_all();
    server_.char_store().SaveState(char_uid_, zone, x, y, money_, level_, exp_, hp_, mp_);
    issue_game_ticket();                               // the 156 of the new connection takes it

    PacketBuffer p(Opcode::S2C_RELOCATE, is_paula());
    p.write<uint32_t>(0);                              // token (0: the 156 is bound by the game_tickets row)
    p.write<uint32_t>(char_uid_);                      // chrUid lo
    p.write<uint32_t>(0);                              // chrUid hi (u64)
    const std::string& ip = server_.game_ip();
    p.write_bytes(reinterpret_cast<const uint8_t*>(ip.c_str()), ip.size() + 1);  // dest IP
    p.write<uint32_t>(server_.game_port());            // dest port
    p.write<uint32_t>(0);                              // reserved
    p.write<uint16_t>(static_cast<uint16_t>(zone));    // zoneTo
    p.write<uint16_t>(static_cast<uint16_t>(x));       // startPos X (MapCoord = x u16 + y u16)
    p.write<uint16_t>(static_cast<uint16_t>(y));       // startPos Y
    p.write<uint8_t>(change_type);                     // changeZoneType (1 town restart, 2 portal)
    send(p);

    LOG_INFO("GameClientSession %u: RELOCATE(154) -> zone=%d (%d,%d); destination persisted, "
             "awaiting reconnect+156", session_id(), zone, x, y);
}

void GameClientSession::handle_enter_zone_cmd(PacketBuffer& pkt)
{
    // C2S 156 (CMD_ENTER_ZONE): first packet on the fresh connection the client opens after a
    // RELOCATE (154). Per the REAL GameServer (EnterZone sub_7FDA00 + caller case 0xBC2, spec 08),
    // a field re-enter is LIGHT: it does NOT re-run the login spawn batch, does NOT re-send session
    // params, and does NOT send 0x0135. The client kept its self-avatar across the 154 (the client
    // 154 handler sub_6ED790 reuses *(GameWorld+1384), it does not rebuild), so re-sending the full
    // batch is exactly what corrupts it ("walk in place"). The real re-enter sends only:
    //     S2C 437 [u16 objId][u16 curX][u16 curY][u32 0]   then   S2C 157 [u32 0].
    // objId must be the char's LOGIN eid (stable per char_uid) so it matches the avatar the client
    // kept — otherwise our move-echo id won't match the client's self id.
    LOG_DEBUG("GameClientSession %u: CMD_ENTER_ZONE(156) raw (%zu bytes): %s", session_id(), pkt.size(),
              hex_bytes(pkt.data(), pkt.size(), 32).c_str());
    // History: an earlier light re-enter (437+157) left the client unable to move, so this path
    // re-ran the whole login batch instead. That failure dated from the old "one-behind" framing
    // (every packet carried a non-zero block id, so the client held the LAST packet — here 157, the
    // enter-OK — until another one arrived); with block id 0 the light path is applied at once. The
    // full batch never sent the appear exchange either (the client sends no C2S 0x0005 on this path,
    // and that was where we exchanged appears) — the "limbo" after a death restart.
    //
    // Retail sequence (Master ack 3010, GameServer case 0xBC2 + EnterZone sub_7FDA00):
    //   S2C 437 [u16 objId][pos][u32 0]            to the client (+ the KR client's trailing u8)
    //   EnterZone: sector enter = mutual op-8 appears with the players + the monster/NPC appears
    //              (sub_7EEE10), then sub_5F6FC0 = our appear as op 9 (warp-in effect) to the area
    //   S2C 157 [u32]                               to the client: enter OK
    // No 0x2A7/0x00E6/0x00E7/0x00E9/0x0005 and no 0x0135 — the client keeps its in-world state.
    try {
        const uint32_t uid = pkt.read<uint32_t>();   // [0..3] chrUid token (same prefix as 0x0004 auth)

        // Hardening (not retail): one character per connection — a second 0x0004 / 156 closes it.
        if (char_uid_) {
            LOG_WARN("GameClientSession %u: a second enter (C2S 156, char %u) on the connection of char %u - "
                     "closed", session_id(), uid, char_uid_);
            close();
            return;
        }
        // The ticket our 154 wrote (issue_game_ticket), for this address (release spec); none: nothing is
        // sent and the connection closes.
        uint32_t ticket_account = 0;
        if (!take_game_ticket(uid, ticket_account)) {
            close();
            return;
        }
        char_uid_ = uid;
        // A double login (GSC-04) — never the 154's old connection (relocated: kick_char skips it, and with no other
        // holder our client's community link is left alone); a racing 0x0004 session's links from elsewhere close.
        server_.kick_char(char_uid_, session_id(), peer_ip_, /*community*/true);

        if (!load_char_from_db()) {         // loads the destination zone/pos persisted at warp time
            char_uid_ = 0;
            return;
        }
        if (account_id_ != ticket_account) {
            LOG_WARN("GameClientSession %u: char %u belongs to account %u, not to the ticket's %u - closed",
                     session_id(), char_uid_, account_id_, ticket_account);
            char_uid_ = 0;
            close();
            return;
        }
        update_return_town(/*at_login*/false);  // Player vtbl+60 on entering the zone
        cdm_restore_carry();                // the Crazy Drill state of the zone we left (drill.cpp)
        monq_restore_carry();               // and the monster quest's (monster_quest.cpp)
        pq_restore_carry();                 // and the party quest's (party_quest.cpp)
        es_restore_carry();                 // and the effect states (es.cpp; the avatar kept their looks)
        if (const int32_t m = server_.disguise_take(char_uid_)) {   // and a disguise (sub_769530 + StateType 16)
            set_disguise_model(m);
            if (disguised_) state_type_ = 2 | 16;
        }
        enter_zone(zone_id_, pos_x_, pos_y_);   // stable per-char eid (matches client's kept self-id)
        server_.party().enter(char_uid_, session_id(), party_info(), is_paula());   // back in its party (S2C 7036)

        // S2C 437 (0x01B5): VERIFIED in the client (sub_6F0960 case 437): u16 id + packed pos ->
        // local player sub_6B1C10 (id + position), sub_6AEC40 reads u32 flag (0 = no extra block),
        // then u8 -> GameWorld+0x10AC0. The readers throw on a short packet, so the u8 is included.
        {
            PacketBuffer e(static_cast<Opcode>(0x01B5), is_paula());
            e.write<uint16_t>(static_cast<uint16_t>(object_id_));
            e.write<uint16_t>(static_cast<uint16_t>(pos_x_));
            e.write<uint16_t>(static_cast<uint16_t>(pos_y_));
            e.write<uint32_t>(0);           // sub_76B0B0: no special state -> u32 0
            e.write<uint8_t>(0);            // KR client trailing u8
            send(e);
        }

        state_ = GameClientSessionState::InWorld;
        ensure_world_tick();
        // Case 0xBC2, between the 437 and EnterZone: the equipment's OnLoad (sub_40C0F0 -> sub_75DEA0) — an
        // equipped pet item makes its pet, op 8 to its area: the others; the sector enter below shows it to us.
        pet_load(/*to_self*/false);
        protect(3000);                      // EnterZone: sub_73CF80(3000, 0) — 3000 ms of protection
        server_.player_entered(*this);      // sector enter: mutual appears + monsters/NPCs (+ the pets)

        // sub_5F6FC0: our appear as op 9 (warp-in effect) to the area. VERIFIED: client sub_6F0960
        // case 9 -> sub_6F02B0(args, 1): same body as op 8, plus the "dung jang"/"etc_warp" effect.
        {
            PacketBuffer ap = build_appear(0x0009);
            server_.broadcast_zone(zone_id_, ap, 0);
        }

        // S2C 157 (0x009D): VERIFIED in the client (case 157): clears the local player's transition
        // bit (+0x6790 &= ~0x20) and reads one u32 (0 = nothing to show).
        {
            PacketBuffer ok(static_cast<Opcode>(0x009D), is_paula());
            ok.write<uint32_t>(0);
            send(ok);
        }
        crazy_restore_carry();              // the Bear's Crazy Hammer gauge / mode (skill_bear.cpp)
        post_login();                       // case 156: MCmd 3156, the post box counts (postbox.cpp)
        LOG_INFO("GameClientSession %u: 156 re-enter (EnterZone: 437 + appears + 9 + 157) char %u -> "
                 "zone=%d (%d,%d) eid=%u", session_id(), char_uid_, zone_id_, pos_x_, pos_y_,
                 static_cast<unsigned>(object_id_));
    } catch (const std::exception& e) {
        LOG_ERROR("GameClientSession %u: 156 re-entry error: %s", session_id(), e.what());
        close();
    }
}

// The Login -> Game handoff ticket (release spec; CORE-02 / GSC-01). The LoginServer at char select, and we
// before a reconnect we order (issue_game_ticket), write a game_tickets row {char uid, account, client address,
// expiry}. C2S 0x0004 / 156 enter only with a live row of that character for this connection's address whose
// character the row's account owns; the row is used once. `account` = the ticket's account.
// Hardening (not retail): the retail GameServer was handed the logged-in character by its Master; the char uid
// the client sends, taken alone, let any client enter as any character (GM ones included).
bool GameClientSession::take_game_ticket(uint32_t uid, uint32_t& account)
{
    Database& db = server_.db();
    const char* why = nullptr;
    try {
        const DBResult rows = db.query(
            "SELECT t.account_id, t.client_ip, (t.expires_at > NOW()) AS live, c.owner "
            "FROM game_tickets t LEFT JOIN characters c ON c.uid = t.char_uid AND c.deleted = 0 "
            "WHERE t.char_uid = %u", uid);
        if (rows.empty())                                         why = "no ticket";
        else if (rows[0].at("client_ip") != peer_ip_)             why = "the ticket is for another address";
        else if (rows[0].at("live") != "1")                       why = "the ticket expired";
        else if (rows[0].at("owner") != rows[0].at("account_id")) why = "the character is not the ticket account's";
        else if (db.execute("DELETE FROM game_tickets WHERE char_uid = %u AND client_ip = '%s' AND expires_at > NOW()",
                            uid, db.escape(peer_ip_).c_str()) != 1)
            why = "the ticket was used meanwhile";
        else
            account = static_cast<uint32_t>(std::stoul(rows[0].at("account_id")));
    } catch (const std::exception& e) {
        LOG_ERROR("GameClientSession %u: game_tickets: %s (sql/019_game_tickets.sql applied?)", session_id(), e.what());
        why = "the ticket could not be read";
    }
    if (why) {
        LOG_WARN("GameClientSession %u: char %u refused from %s: %s", session_id(), uid, peer_ip_.c_str(), why);
        return false;
    }
    return true;
}

// Every reconnect we order (S2C 154 RELOCATE: the client comes back on a new connection with C2S 156) is
// preceded by a fresh ticket for this character, its account and our client's address — the LoginServer's
// kind of row (5 minutes, one use).
void GameClientSession::issue_game_ticket()
{
    Database& db = server_.db();
    try {
        db.execute("REPLACE INTO game_tickets (char_uid, account_id, client_ip, expires_at) "
                   "VALUES (%u, %u, '%s', NOW() + INTERVAL 300 SECOND)",
                   char_uid_, account_id_, db.escape(peer_ip_).c_str());
    } catch (const std::exception& e) {
        LOG_ERROR("GameClientSession %u: the game ticket of char %u was not written: %s", session_id(), char_uid_,
                  e.what());
    }
}

bool GameClientSession::load_char_from_db()
{
    // account_id_ is not sent in 0x0004 — query by char uid alone (take_game_ticket bound that uid to its
    // account and this address first).
    // Loads all char_attr, char_state, and char_status fields in one query.
    auto rows = server_.db().query(
        "SELECT owner, name, type, job, f_type, type_2, job_2, type_3, job_3, change_job_queststate, "
        "       pts_p, pts_m, pts_s, pts_c, "
        "       level, exp, hp, hp_max, mp, mp_max, money, "
        "       zone, pos_x, pos_y, home, state, authority, "
        "       hair, hair_2, hair_3, job_state, wh_slots, "
        "       ap_pts, ac_pts, dx_pts, ma_pts, md_pts, "
        "       da_pts, lk_pts, dp_pts, hv_pts, "
        "       max_hp_pts, max_mp_pts, max_wt_pts, attr_pts, tm_level, tm_exp, tm_point "
        "FROM characters WHERE uid=%u AND deleted=0",
        char_uid_
    );

    if (rows.empty()) {
        LOG_ERROR("GameClientSession %u: char %u not found in DB", session_id(), char_uid_);
        close();
        return false;
    }

    auto& r     = rows[0];
    account_id_ = static_cast<uint32_t>(std::stoul(r.at("owner")));
    authority_  = static_cast<uint8_t>(std::stoi(r.at("authority")));
    zone_id_    = static_cast<int16_t>(std::stoi(r.at("zone")));
    pos_x_      = std::stoi(r.at("pos_x"));
    pos_y_      = std::stoi(r.at("pos_y"));
    {   // return town: retail SetChrStateInfo (sub_75D1F0) keeps 1..999, anything else becomes 1
        const int h = std::stoi(r.at("home"));
        home_ = static_cast<int16_t>((h < 1 || h >= 1000) ? 1 : h);
    }
    money_      = std::stoi(r.at("money"));
    base_type_  = static_cast<int16_t>(std::stoi(r.at("type")));
    type2_      = static_cast<int16_t>(std::stoi(r.at("type_2")));
    type3_      = static_cast<int16_t>(std::stoi(r.at("type_3")));
    job1_       = static_cast<uint8_t>(std::stoi(r.at("job")));
    job2_       = static_cast<uint8_t>(std::stoi(r.at("job_2")));
    job3_       = static_cast<uint8_t>(std::stoi(r.at("job_3")));
    cj_state_   = static_cast<uint8_t>(std::stoi(r.at("change_job_queststate")));
    f_type_     = static_cast<uint8_t>(std::stoi(r.at("f_type")));
    pts_p_      = static_cast<uint8_t>(std::stoi(r.at("pts_p")));
    pts_m_      = static_cast<uint8_t>(std::stoi(r.at("pts_m")));
    pts_s_      = static_cast<uint8_t>(std::stoi(r.at("pts_s")));
    pts_c_      = static_cast<uint8_t>(std::stoi(r.at("pts_c")));
    level_      = static_cast<int16_t>(std::stoi(r.at("level")));
    exp_        = std::stoi(r.at("exp"));
    hp_         = std::stoi(r.at("hp"));
    hp_max_     = std::stoi(r.at("hp_max"));   // overridden by recompute_stats() below (gulim13 is authoritative)
    mp_         = std::stoi(r.at("mp"));
    mp_max_     = std::stoi(r.at("mp_max"));
    hair_       = static_cast<uint8_t>(std::stoi(r.at("hair")));
    hair2_      = static_cast<uint8_t>(std::stoi(r.at("hair_2")));
    hair3_      = static_cast<uint8_t>(std::stoi(r.at("hair_3")));
    job_state_  = static_cast<uint8_t>(std::stoi(r.at("job_state")));
    apply_job_state();
    // The warehouse's tabs from char_state.wh_slots (SetChrStateInfo sub_75D1F0 -> sub_6F2BA0; a
    // warehouse already there is dropped first). Its items are read on its first opening.
    wh_tabs_.clear();
    wh_slots_ = 0;
    wh_use_   = 0;
    wh_state_ = false;
    wh_build(static_cast<uint16_t>(std::stoi(r.at("wh_slots"))));
    // The bank (the account's NeoWarehouse): read on its first opening of this Player.
    bank_.clear();
    bank_weight_ = 0;
    bank_money_  = 0;
    bank_sent_   = false;
    bank_state_  = false;
    // Sub-stats
    ap_pts_     = static_cast<int16_t>(std::stoi(r.at("ap_pts")));
    ac_pts_     = static_cast<int16_t>(std::stoi(r.at("ac_pts")));
    dx_pts_     = static_cast<int16_t>(std::stoi(r.at("dx_pts")));
    ma_pts_     = static_cast<int16_t>(std::stoi(r.at("ma_pts")));
    md_pts_     = static_cast<int16_t>(std::stoi(r.at("md_pts")));
    da_pts_     = static_cast<int16_t>(std::stoi(r.at("da_pts")));
    lk_pts_     = static_cast<int16_t>(std::stoi(r.at("lk_pts")));
    dp_pts_     = static_cast<int16_t>(std::stoi(r.at("dp_pts")));
    hv_pts_     = static_cast<int16_t>(std::stoi(r.at("hv_pts")));
    max_hp_pts_ = static_cast<int16_t>(std::stoi(r.at("max_hp_pts")));
    max_mp_pts_ = static_cast<int16_t>(std::stoi(r.at("max_mp_pts")));
    attr_pts_   = static_cast<int16_t>(std::stoi(r.at("attr_pts")));
    std::strncpy(char_name_, r.at("name").c_str(), 17);
    char_name_[17] = '\0';

    {
        int tm = std::stoi(r.at("tm_level"));       // TM 0 is valid (retail tables index 0..500)
        tm_level_ = static_cast<int16_t>(tm < 0 ? 0 : (tm > 500 ? 500 : tm));
        tm_exp_    = std::max(0, std::stoi(r.at("tm_exp")));
        tm_points_ = std::max(0, std::stoi(r.at("tm_point")));
    }

    // Accumulated stat points: use the stored ones if they satisfy the retail invariant (sub_75CE00 /
    // sub_73C7A0: every slot >= 0 and Σ12 + unspent == 34*(level-1)); otherwise re-derive them from the
    // growth graph (fresh / never-saved / inconsistent char). Then derive the effective stats; this
    // overrides the DB hp_max/mp_max with the authoritative gulim13 values.
    {
        const int32_t stored[ST_COUNT] = {        // canonical order
            ap_pts_, ac_pts_, dx_pts_, max_mp_pts_, ma_pts_, md_pts_,
            static_cast<int32_t>(std::stoi(r.at("max_wt_pts"))), da_pts_, lk_pts_,
            max_hp_pts_, dp_pts_, hv_pts_,
        };
        long sum = attr_pts_;
        bool valid = attr_pts_ >= 0;
        for (int i = 0; i < ST_COUNT; ++i) { if (stored[i] < 0) valid = false; sum += stored[i]; }
        const long expect = 34L * ((level_ > 1) ? (level_ - 1) : 0);
        if (valid && sum == expect) {
            for (int i = 0; i < ST_COUNT; ++i) accpts_[i] = stored[i];
            recompute_stats(false);                 // no clamp yet: the equipment comes with the bag
        } else {
            LOG_INFO("GameClientSession %u: stored stat points inconsistent (sum %ld != 34*(lv-1)=%ld) — "
                     "re-deriving from growth", session_id(), sum, expect);
            init_accpts_from_growth(false);
        }
    }
    // Client::SetPlayerFromLoginInfo: a char that logs in dead (HP <= 0) comes back with 30% HP.
    if (hp_ <= 0) hp_ = 30 * hp_max_ / 100;

    // Learned skills (at most 80, as SetPlayerFromLoginInfo; ids not in SkillParam2 are skipped —
    // the retail sub_8922B0 / client sub_71EC60 both drop an id their skill table doesn't know).
    skills_.clear();
    {
        auto srows = server_.db().query(
            "SELECT skill_id, level FROM char_skills WHERE char_uid=%u ORDER BY skill_id", char_uid_);
        for (auto& sr : srows) {
            if (skills_.size() >= 80) break;
            const int sid = std::stoi(sr.at("skill_id"));
            const int lv  = std::stoi(sr.at("level"));
            if (!server_.skill_db().get(sid)) continue;
            skills_.push_back({ static_cast<uint16_t>(sid), static_cast<uint8_t>(lv) });
        }
    }
    // The bag (SetPlayerFromLoginInfo: the char record's item list, at most 300) and the equipment
    // (sub_75DB60: the 23 equipped uids), whose stats join the final stats.
    load_inventory();
    recompute_stats();
    // The normal quests in progress (char_quest types 1 / 4 -> the QuestInv, quest.cpp) and the monster
    // quest (types 2 / 5 -> the MonsterQuestInv, monster_quest.cpp).
    load_quests();
    load_monster_quest();
    load_skill_quests();                    // the change-job quests (char_quest type 3, skill_shop.cpp)
    pq_load();                              // the party quest whose reward waits (party_quest.cpp)

    LOG_INFO("GameClientSession %u: loaded char %u ('%s') account=%u zone=%d lv=%d hp=%d/%d mp=%d/%d",
             session_id(), char_uid_, char_name_, account_id_, zone_id_,
             level_, hp_, hp_max_, mp_, mp_max_);
    return true;
}

// Player::SetJobState (sub_761250): the tier the character shows, job_state 1..3, picks the model
// (Player +436 = type / type_2 / type_3) and the hair (+440 = hair / hair_2 / hair_3); the class
// (+1024) is always the highest tier reached — job_3, else job_2, else job — whatever is shown. (The
// retail refuses a model id >= 25 — SetMakeUp error; a shown tier without a model shows the 1st
// here.) The 0x02A7 blob and the 0x00E9 block carry all three tiers; the client picks the same way
// (Build_Local_Avatar sub_6E1340).
void GameClientSession::apply_job_state()
{
    type_ = base_type_;
    if (job_state_ == 2 && type2_ > 0)      type_ = type2_;
    else if (job_state_ == 3 && type3_ > 0) type_ = type3_;
    job_ = job3_ ? job3_ : (job2_ ? job2_ : job1_);
}

uint8_t GameClientSession::shown_hair() const
{
    if (job_state_ == 2 && type2_ > 0) return hair2_;
    if (job_state_ == 3 && type3_ > 0) return hair3_;
    return hair_;
}

void GameClientSession::save_position()
{
    if (!char_uid_) return;
    // Hardening (not retail): our exchange's DB step runs (trade.cpp) — our gelders here are from before it, and
    // the step writes them itself; the save waits (the next one, or on_close after trade_close).
    if (trade_sync_) return;
    // Persist the full mutable progress (position + money/level/exp/hp/mp), not just x/y,
    // so nothing is lost on zone-leave / logout. procs->methods via CharStore.
    server_.char_store().SaveState(char_uid_, zone_id_, pos_x_, pos_y_,
                                   money_, level_, exp_, hp_, mp_);
    server_.char_store().SaveHome(char_uid_, home_);
    save_stat_points();
}

// The return town (retail Player +1656). Entering a zone whose MapInfoEx Type is 1 or 2 (towns /
// town entrances) and whose Class is not 3 makes it the return town — Player vtbl+60 (sub_73AA70) on
// every zone entry; at login (Client::SetPlayerFromLoginInfo) Class 5 is excluded as well.
void GameClientSession::update_return_town(bool at_login)
{
    const MapDef* z = server_.maps_db().get(zone_id_);
    if (!z || home_ == zone_id_) return;
    if (!z->is_return_town()) return;
    if (at_login && z->cls == 5) return;
    home_ = zone_id_;
    server_.char_store().SaveHome(char_uid_, home_);
    LOG_INFO("GameClientSession %u: return town -> zone %d", session_id(), home_);
}

// ─── Responses ───────────────────────────────────────────────────────────────

void GameClientSession::send_session_open()
{
    // 0x1582 SESSION_OPEN: sub_B33530 in the client reads this as u16 count of
    // cGuildBattleManager::sGameStepInfo entries (each entry = u8 + u16 + string).
    // Sending count=0 avoids any loop iterations and any possible out-of-bounds reads.
    PacketBuffer pkt(Opcode::SESSION_OPEN, is_paula());
    pkt.write<uint16_t>(0);   // entry count = 0 (no guild battle step info)
    send(pkt);
}

void GameClientSession::send_map_enter_ack()
{
    // 0x0005 S2C: reply to C2S 0x0004 auth.
    // LoadingTask handler (sub_9C0DF0) reads u8: 0 = success.
    // On success it calls Task_SetNextState(1) → NewGameTask_Init immediately.
    PacketBuffer pkt(Opcode::MAP_ENTER_ACK, is_paula());
    pkt.write<uint8_t>(0);   // 0 = enter OK
    send(pkt);
    LOG_DEBUG("GameClientSession %u: sent 0x0005 MAP_ENTER_ACK for char %u",
             session_id(), char_uid_);
}

void GameClientSession::send_session_param_1()
{
    // 0x1585 (5509): where the community server (the retail SystemServer) is. Client dispatch_packet_main
    // case 0x1585 -> session_param_5509 (0x83DAC0) reads [u64 key][str ip][u32 port][u32 crc] and opens a
    // SECOND connection to ip:port (c2s_5505 -> sub_82A560 connect), then logs in on it with C2S 0x1581
    // (5505) [u64 key][u32 crc] -> S2C 0x1582 (5506), the community flag (byte_11364B8) the party window
    // needs (system_session.cpp). The key is the character uid, the crc a one-time ticket. (An empty ip
    // / port 0 — what this used to send — fails the connect: "Party system is not available due to
    // disconnection from the Server".)
    if (!char_uid_) return;
    const std::string& ip = server_.system_ip();
    PacketBuffer pkt(Opcode::SESSION_PARAM_1, is_paula());
    pkt.write<uint64_t>(char_uid_);
    pkt.write_bytes(ip.c_str(), ip.size() + 1);
    pkt.write<uint32_t>(server_.system_port());
    pkt.write<uint32_t>(server_.system_ticket(char_uid_));
    send(pkt);
    LOG_DEBUG("GameClientSession %u: sent 0x1585 community server %s:%u", session_id(), ip.c_str(),
             server_.system_port());
}

void GameClientSession::send_session_param_2()
{
    // 0x1586 SESSION_PARAM_2: companion to PARAM_1. Exact layout unknown;
    // sending zero payload (empty body) as a safe dummy.
    PacketBuffer pkt(Opcode::SESSION_PARAM_2, is_paula());
    send(pkt);
    LOG_DEBUG("GameClientSession %u: sent 0x1586 SESSION_PARAM_2", session_id());
}

void GameClientSession::send_party_init()
{
    // 0x1773 PARTY_DATA_UPDATE: full party state — "8 sub-objects" (8 member slots).
    // Sending count=0 (no party members) to initialize the party/SoulSeed system
    // to an empty state, preventing SoulSeedUICtrl from reading uninitialized data.
    PacketBuffer pkt(Opcode::PARTY_DATA_UPDATE, is_paula());
    pkt.write<uint16_t>(0);   // member count = 0
    send(pkt);
    LOG_DEBUG("GameClientSession %u: sent 0x1773 PARTY_DATA_UPDATE (empty)", session_id());
}

void GameClientSession::send_social_init()
{
    // 0x2AFA SOCIAL_INIT: initializes the social/pairing system that SoulSeedUICtrl
    // depends on. Sending count=0 (no pairs) as a safe dummy.
    PacketBuffer pkt(Opcode::SOCIAL_INIT, is_paula());
    pkt.write<uint16_t>(0);   // entry count = 0
    send(pkt);
    LOG_DEBUG("GameClientSession %u: sent 0x2AFA SOCIAL_INIT (empty)", session_id());
}

void GameClientSession::send_player_spawn()
{
    // 0x0007 PLAYER_SPAWN_LOGIN (KR layout) — the frame that actually renders the
    // player in the world. Layout: Header(22) + NUL-terminated name + compact footer(71).
    PacketBuffer pkt(Opcode::PLAYER_SPAWN_LOGIN, is_paula());
    const uint16_t eid     = static_cast<uint16_t>(object_id_);
    const uint16_t species = static_cast<uint16_t>(type_);

    // ── Header (0x16 = 22 bytes) ──
    pkt.write<uint16_t>(eid);                              // 0x00 EntityId
    pkt.write<uint16_t>(1);                                // 0x02 AppearanceType = 1 (Player)
    pkt.write<uint16_t>(species);                          // 0x04 BodyType = species
    pkt.write<uint8_t>(hair_);                             // 0x06 Hair
    pkt.write<uint8_t>(static_cast<uint8_t>(species));     // 0x07 ClassChainId = species
    pkt.write<uint16_t>(static_cast<uint16_t>(level_));    // 0x08 Level
    pkt.write<uint32_t>(8);                                // 0x0A UnknownA = 8 (const)
    pkt.write<uint16_t>(static_cast<uint16_t>(pos_x_));    // 0x0E PositionX
    pkt.write<uint16_t>(static_cast<uint16_t>(pos_y_));    // 0x10 PositionY
    pkt.write<uint16_t>(static_cast<uint16_t>(pos_x_));    // 0x12 RespawnX
    pkt.write<uint16_t>(static_cast<uint16_t>(pos_y_));    // 0x14 RespawnY

    // ── Name + single NUL terminator ──
    size_t nameLen = std::strlen(char_name_);
    pkt.write_bytes(char_name_, nameLen);
    pkt.write<uint8_t>(0);

    // ── Compact footer (71 bytes, zero-init, specific fields set) ──
    uint8_t footer[71] = {};
    uint32_t v;
    footer[1] = 0;                                         // f+1  EncodedYaw = 0 (facing north)
    v = 1;            std::memcpy(&footer[7],  &v, 4);     // f+7  UnknownB = job_state
    v = species;      std::memcpy(&footer[21], &v, 4);     // f+21 ClassChainMirror = species
    v = 0x00006000u;  std::memcpy(&footer[25], &v, 4);     // f+25 PledgeMarker (KR default)
    v = 0;            std::memcpy(&footer[30], &v, 4);     // f+30 EquipEar
    v = 0;            std::memcpy(&footer[34], &v, 4);     // f+34 EquipTail
    footer[42] = hair_;                                    // f+42 Hair (re-emitted)
    pkt.write_bytes(footer, sizeof(footer));

    send(pkt);
    LOG_INFO("GameClientSession %u: sent PLAYER_SPAWN_LOGIN (0x0007) eid=%u species=%u '%s'",
             session_id(), eid, species, char_name_);
}

void GameClientSession::send_entity_id_echo()
{
    // 0x03A5 ENTITY_ID_ECHO — the client asks "is this me?"; echoing the entityId
    // binds the freshly-spawned entity to the local player so it can be controlled.
    PacketBuffer pkt(Opcode::ENTITY_ID_ECHO, is_paula());
    pkt.write<uint16_t>(static_cast<uint16_t>(object_id_));
    send(pkt);
    LOG_DEBUG("GameClientSession %u: sent ENTITY_ID_ECHO (0x03A5) eid=%u",
              session_id(), static_cast<uint16_t>(object_id_));
}

// The 69-byte character info (S2C 0x02A7 at login, and S2C 649 after a growth graph reset — the client
// copies both to GameWorld+10440).
void GameClientSession::build_char_blob(uint8_t blob[69]) const
{
      std::memcpy(&blob[0], &char_uid_, 4);                 // +0  char UID lo (hi=0)
      std::strncpy(reinterpret_cast<char*>(&blob[17]),
                   char_name_, 20);                          // +17 char name (<=20)
      // AUTHORITATIVE layout from the MasterServer: cCharStore::LoadChrInfo (sub_4D9B20)
      // SQLBindCol's the uspm_char_all_select_char_info columns to the struct that becomes
      // this 69-byte blob. Column order (SQL_Bak\trickster_modiy.sql):
      //   +37 type  +39 job  +40 f_type  +41..44 pts_p/m/s/c  +53 type_2  +55 job_2
      //   +56 type_3  +58 job_3  (hair/hair_2/hair_3 are at char+156/157/161 — NOT in this blob)
      // The client (Build_Local_Avatar sub_6E1340) takes the model of the shown tier (0x00E9
      // block+56 job_state: +37 / +53 / +56) and the class job_3 ?: job_2 ?: job. A tier without a
      // model gets the 1st tier's one, so a stray job_state never reads model 0 (the garbage-palette
      // crash, sub_6CA250 -> sub_56BD90).
      const uint16_t t1 = static_cast<uint16_t>(base_type_);
      const uint16_t t2 = static_cast<uint16_t>(type2_ > 0 ? type2_ : base_type_);
      const uint16_t t3 = static_cast<uint16_t>(type3_ > 0 ? type3_ : base_type_);
      std::memcpy(&blob[37], &t1, 2);          // +37 type
      std::memcpy(&blob[53], &t2, 2);          // +53 type_2
      std::memcpy(&blob[56], &t3, 2);          // +56 type_3
      blob[39] = job1_;                        // +39 job
      blob[55] = job2_;                        // +55 job_2 (0 = none)
      blob[58] = job3_;                        // +58 job_3 (0 = none)
      blob[40] = f_type_;                      // +40 f_type (gender)
      // +41..+44 = the 4 Growth/Compose values (Power/Magic/Sense/Charm). Confirmed in the
      // client: sub_A44670 reads GameWorld+10481..10484 (= blob+41..44), scales each by 0.25
      // (max 4 → full bar) and draws the 12-segment Growth graph; Build_Local_Avatar
      // (sub_6E1340) also feeds them into the avatar appearance block (sub_49EB10). Sending 0
      // here was the "0 0 0 0" growth. DB pts_p/m/s/c = the bear's canonical 4/1/3/2.
      // RESTORED 2026-10-14: the 2026-10-08 diagnostic that zeroed these (to test a movement
      // gate) was never reverted — that is the "0 0 0 0" growth the player saw. The client reader
      // is sub_A44670 (GameWorld+10481..10484), so these are the authoritative growth values.
      blob[41] = static_cast<uint8_t>(pts_p_);  // +41 Power growth
      blob[42] = static_cast<uint8_t>(pts_m_);  // +42 Magic growth
      blob[43] = static_cast<uint8_t>(pts_s_);  // +43 Sense growth
      blob[44] = static_cast<uint8_t>(pts_c_);  // +44 Charm growth
}

void GameClientSession::send_world_spawn()
{
    // Full KR in-world spawn sequence (Zuri spec). Sent after 0x0004 map auth.
    // NOTE: handle_auth already sent 0x0005 (MAP_ENTER_ACK == GAME_PHASE_2) + the
    // session params + CHAR_INFO to transition the client into NewGameTask (that
    // prefix reached 100% zone load). This fills in the Phase-1 data batch and the
    // Phase-2 self-spawn the client needs so its entity/worker threads don't deref NULL.
    //
    // Layouts marked BEST-EFFORT are inferred (not yet verified against the client parser)
    // — refine via the test loop / IDA if a specific packet crashes.
    using O = Opcode;
    const uint16_t eid     = static_cast<uint16_t>(object_id_);
    const uint16_t species = static_cast<uint16_t>(type_);
    const uint32_t now     = static_cast<uint32_t>(std::time(nullptr));

    auto empty_list = [this](O op){ PacketBuffer p(op, is_paula()); p.write<uint16_t>(0); send(p); };

    // ═════════ EXACT login-enter sequence from the real GameServer.exe ═════════
    // Reverse-engineered from Zone::PrepareToStartPlayer → sub_5AAE70 in the original
    // UNPROTECTED server: it sends these 10 packets to the client IN THIS ORDER, then
    // opcode 0x0005 LAST as the "enter now" trigger. The server sends NO 0x0007 / 0x03A5
    // / float-arrays / 0x017C at login — those were our guesses and are removed.
    // Server send prims: sub_43C070(char,opcode,buf)=payload, sub_454070(char,opcode)=empty.
    // (void)now/eid/species kept for the payload builders below.
    (void)now;

    // #1  679/0x02A7 GAME_CHAR_DATA — EXACTLY 69 bytes, memcpy'd to G+10440 in the
    //     client (sub_7457A0 case 679). Verified field layout from the client parser +
    //     avatar builder sub_6E1340 / cash-shop URL builder sub_41CD90:
    //       [+0 ] char UID (u64)          [+17] char name (20 bytes, strncpy 0x20)
    //       [+37] species/appearance (u16) [+39] hair    [+40] body/PMSC (f_type)
    //     A wrong SIZE makes the client read 69 bytes of adjacent buffer garbage, so
    //     this MUST be 69 bytes exactly (was ~33 → zone/appearance came out as garbage).
    { PacketBuffer p(O::GAME_CHAR_DATA, is_paula());
      uint8_t blob[69] = {};
      build_char_blob(blob);
      p.write_bytes(blob, sizeof(blob));
      send(p); }
    // #2  230/0x00E6 GAME_BATCH_HEADER — EXACTLY 10 bytes (sub_6E2360):
    //       [u32 →G+10552][u16 entityId →G+10560][u16 X →G+10562][u16 Y →G+10564]
    { PacketBuffer p(O::GAME_BATCH_HEADER, is_paula());
      p.write<uint32_t>(0);                                  // G+10552 (avatar/time id)
      p.write<uint16_t>(eid);                                // entityId
      p.write<uint16_t>((uint16_t)pos_x_);                   // X
      p.write<uint16_t>((uint16_t)pos_y_);                   // Y
      send(p); }
    // #3  231/0x00E7 — the ITEM list (every item of the character; sub_5F69D0 / client sub_6E23D0).
    send_item_list();
    // #4  233/0x00E9 — EXACTLY 278 bytes (sub_6DFB90). NOT just stats — it carries the
    //     ZONE and the APPEARANCE MODE. Handler stores: 23 qwords→G+17772 (equip slots),
    //     33 bytes→G+17956, then 61 bytes→G+17989. The 61-byte section is the player
    //     transform/appearance region:
    //       byte 219 (section+2,  u16) = G+17991 = ZONE id      → enter-field loads this map
    //       byte 271 (section+54, u8 ) = G+18043 = avatar 2nd param (mode-1) ~ f_type
    //       byte 273 (section+56, u8 ) = G+18045 = APPEARANCE MODE (1 = normal → species
    //                                               read from 0x2A7 blob+37; 0/garbage →
    //                                               uninitialized species → palette crash)
    //     Equip qwords left 0 (no equipment). This fixes BOTH the zone=0 camera crash and
    //     the avatar/palette crash (player+448 species index was garbage from mode=0).
    { PacketBuffer p(O::GAME_CHAR_STATS, is_paula());
      uint8_t e9[278] = {};
      // ── 23 equipped item uids @ e9[0..183] (retail ChrLoginInfo +11244, filled from the DB's
      //    item_where 0 rows at item_pos; client -> G+17772, the avatar's slots)
      std::memcpy(&e9[0], equip_, sizeof(equip_));
      // slot 20 (item_pos 20 in the DB) holds the common slot: the loaded bullets or the throw item
      { const uint64_t common = mag_.item ? mag_.item : throw_item_;
        std::memcpy(&e9[20 * 8], &common, 8); }
      // ── 33-byte STATS section @ e9[184..216] (handler sub_6DFB90 -> player+17956,
      //    processed by sub_6ABC20). Decoded as an array of u16 (a2[0..15]):
      //      a2[0..12] = the 13 stat POINTS, in char_status column order:
      //                  ap, ac, dx, max_mp, ma, md, max_wt, da, lk, max_hp, dp, hv, attr.
      //        These are DERIVED, not stored: each sub-stat gets growthRatio*(level-1)
      //        points (ap/ac/dx=Power, max_mp/ma/md=Magic, max_wt/da/lk=Sense,
      //        max_hp/dp/hv=Charm) and attr=4*(level-1). The client shows the stat LEVEL
      //        = points/4. Sum = (3*(P+M+S+C)+4)*(level-1) = 34*(level-1), which the
      //        server validator (sub_50DDF0) requires (growth always sums to 10).
      //      a2[13] = unused.
      //      a2[14] (e9[212]) = TM (Master) level -> HUD "TM n".
      //      a2[15] (e9[214]) = LEVEL  -> HUD "Lv n".
      //    (All of a2[0..12] are 0 at level 1, so a fresh char shows only its base stats.)
      //    Same 33-byte layout as the retail serializer sub_75CB40 / write_stat_block(): filled from
      //    the session's real accumulated points + pool + TM + level (loaded from the DB, or derived
      //    from growth if never saved), so the login block and the 0x0275 refresh always agree.
      {
          PacketBuffer blk(static_cast<Opcode>(0), is_paula());
          write_stat_block(blk);
          const size_t hdr = is_paula() ? PacketBuffer::PAULA_HEADER_SIZE : PacketBuffer::STANDARD_HEADER_SIZE;
          std::memcpy(&e9[184], blk.data() + hdr, 33);
      }
      // ── 61-byte block @ e9[217..277]. Layout from the REAL server's applier
      //    Player::SetStateInfo (sub_50E1C0, reads block @ ChrLoginInfo+121):
      //      block+12 (u16)  = zone        -> char+1648
      //      block+18 (u32)  = current HP  -> char+444  (defaults to 30% max if <=0;
      //                        SENDING 0 = HP 0/max = "dead" = movement LOCKED)
      //      block+22 (u32)  = current MP  -> char+448
      //      block+30 (u32)  = EXP         -> char+1012
      //      block+56 (u8)   = appearance mode (1 = normal)
      //    (block start = e9[217], so block+N = e9[217+N].)
      uint16_t zone16 = static_cast<uint16_t>(zone_id_);
      uint16_t home16 = static_cast<uint16_t>(home_);
      std::memcpy(&e9[217 + 12], &home16, 2);          // block+12 return town (sub_75CC70: +1656)
      std::memcpy(&e9[219], &zone16, 2);               // block+2 zone (loads the map)
      // block+14 money (sub_75CC70: +3148) — VERIFIED: client sub_6ABD20 sets avatar+26704 = block+14,
      // the same field S2C 0x0029 sets.
      uint32_t money32 = static_cast<uint32_t>(money_);
      std::memcpy(&e9[217 + 14], &money32, 4);
      uint32_t hp32 = static_cast<uint32_t>(hp_);
      uint32_t mp32 = static_cast<uint32_t>(mp_);
      std::memcpy(&e9[217 + 18], &hp32, 4);            // block+18 current HP (e9[235])
      std::memcpy(&e9[217 + 22], &mp32, 4);            // block+22 current MP (e9[239])
      // block+26 TM EXP (+828) and block+30 EXP (+1012) — VERIFIED: the client's sub_6ABD20 (from
      // Build_Local_Avatar) sets avatar+728 = block+26 and avatar+912 = block+30.
      uint32_t tmexp32 = static_cast<uint32_t>(tm_exp_);
      uint32_t exp32   = static_cast<uint32_t>(exp_);
      std::memcpy(&e9[217 + 26], &tmexp32, 4);
      std::memcpy(&e9[217 + 30], &exp32, 4);
      // block+54 (e9[271]) = C.hair = HAIR COLOR. AUTHORITATIVE: MasterServer char+156
      // (C.hair) -> a4[156] -> GameServer char+175 = block+54; the avatar builder reads it
      // as v56 (the hair param) when the appearance tier (job_state) is 1. We were wrongly
      // putting f_type here (f_type lives in the 0x2A7 blob[40]), so hair color came out 0.
      e9[271] = hair_;                      // block+54 = hair color (C.hair)
      e9[272] = hair2_;                     // block+55 = hair_2 (the 2nd tier's; Build_Local_Avatar G+18044)
      e9[276] = hair3_;                     // block+59 = hair_3 (the 3rd tier's; G+18048)
      // block+56 (e9[273]) = C.job_state = appearance tier (1 = base -> species from blob[37]
      // & hair from block+54). It is NOT a generic "mode" flag; it is the job_state column.
      e9[273] = job_state_;                 // block+56 = job_state (1 for a base char)
      // block+57 (u16, e9[274]) = char_state.wh_slots: the warehouse's slot count — the server builds
      // its tabs from it (SetChrStateInfo -> sub_6F2BA0) and so does the client (sub_6ABD20 ->
      // sub_596F60: tabs 2, 21..29 of 80 slots).
      { const uint16_t whs = static_cast<uint16_t>(wh_slots_);
        std::memcpy(&e9[217 + 57], &whs, 2); }
      p.write_bytes(e9, sizeof(e9));
      send(p); }
    // #5  234/0x00EA — the normal quests in progress (server charRec+11428, {u32 qid, u16 run};
    //     sub_5F6BF0 / client sub_6DFC00).
    send_quest_list();
    // #6  235/0x00EB — the MONSTER QUEST list (NOT skills): its client handler sub_6E5EE0 looks
    //     every id up in the MonsterQuestInfo table (data singleton +0x1240, loader sub_629D10) and
    //     NULL-derefs on a miss — which is why sending skill ids here crashed. Empty.
    send_monster_quest_list();
    // #7  296/0x0128 — the skill (change-job) quests in progress (sub_5F6CB0 / client loading
    //     dispatcher sub_7457A0 -> sub_6DFC60): [u16 n]{u32 qid}.
    send_skill_quest_list();
    // #8  271/0x010F — the LEARNED SKILL list. Server: SetPlayerFromLoginInfo loads 5-byte records
    //     from charRec+12094 ("too many skillCount" past 80), serialized by sub_5F6AA0. Client: the
    //     loading dispatcher sub_7457A0 -> sub_6DFB40 copies [u16 count] + count x 5 bytes into
    //     GameWorld+0xA69C; Build_Local_Avatar -> sub_71EC60 adds each to the player's skills
    //     (id validated against SkillParam2, u8 -> skill level).
    { PacketBuffer p(O::GAME_SKILL_LIST_KR, is_paula());
      p.write<uint16_t>(static_cast<uint16_t>(skills_.size()));
      for (const auto& s : skills_) {
          p.write<uint16_t>(s.id);
          p.write<uint8_t>(s.level);
          p.write<uint16_t>(0);
      }
      send(p);
      LOG_DEBUG("GameClientSession %u: S2C 271 learned skills = %zu", session_id(), skills_.size()); }
    // #9  508/0x01FC GAME_ITEM_EXPIRY (sub_5F6E70): [u64 the server's clock][u16 n][n x 21-byte expiry record].
    //     The u64 is the global FILETIME of the server's local time (sub_508680: GetLocalTime +
    //     SystemTimeToFileTime). VERIFIED: client sub_6DFCB0 keeps it with its own local time at arrival and
    //     its "now" is then local + (server - arrival) (sub_6DEAD0) — the post box's return countdown
    //     (sub_6D3710). Zeros put that "now" in 1601: every post was due at once (-1000 ms after the int
    //     overflow) and the client dropped them as returned ("NotiDelPost") — fixed 2026-10-19.
    { PacketBuffer p(O::GAME_ITEM_EXPIRY, is_paula());
      p.write<uint64_t>(local_filetime_now());
      p.write<uint16_t>(0);                               // no expiring items
      send(p); }
    // #10 833/0x0341 — the scenario quests in progress (sub_6038F0 / client sub_6E0410).
    send_scenario_quest_list();

    // #11 5/0x0005 MAP_ENTER_ACK — the FINAL "enter now" trigger (sub_454070(char,5)).
    send_map_enter_ack();

    LOG_DEBUG("GameClientSession %u: sent REAL login-enter sequence (10 pkts + 0x0005 last, eid=%u species=%u)",
             session_id(), eid, species);
}

void GameClientSession::send_map_change()
{
    // 0x017C S2C_MAP_CHANGE — RE'd in Client_live: in-world dispatcher sub_6F0960
    // (high-opcode table) → sub_6EDB50 → sub_6E80C0(mapId) loads the field and sets
    // the client's pending-zone. Payload = [u16 mapId][u16 posX][u16 posY]
    // (client reads ReadU16 then ReadU16x2). Position is stored at player+108/110.
    PacketBuffer pkt(Opcode::S2C_MAP_CHANGE, is_paula());
    pkt.write<uint16_t>(static_cast<uint16_t>(zone_id_));   // mapId (table index into dword_10988F4)
    pkt.write<uint16_t>(static_cast<uint16_t>(pos_x_));     // posX
    pkt.write<uint16_t>(static_cast<uint16_t>(pos_y_));     // posY
    send(pkt);
    LOG_INFO("GameClientSession %u: sent S2C_MAP_CHANGE (0x017C) mapId=%d pos=(%d,%d)",
             session_id(), zone_id_, pos_x_, pos_y_);
}

PacketBuffer GameClientSession::build_appear(uint16_t opcode) const
{
    // S2C op 8 "remote player appear". Reversed byte-exact from the client:
    //   header  -> sub_6F02B0  (0x6F02B0)
    //   body    -> Player appearance reader sub_6CDF30 (vtbl+56), minimal form
    // All appearance sub-masks are 0 and bodyFlags bit0=1 (chr100 body), so every conditional
    // read is skipped and the byte layout is fixed. kind=1 selects the client Player/Avatar class.
    PacketBuffer p(static_cast<Opcode>(opcode), is_paula());
    const uint16_t eid = static_cast<uint16_t>(object_id_);
    // Retail sub_715550 writes the CURRENT position (sub_4100C0) as pos1, the destination (+120) as
    // pos2 and the actor state flags (+92) whose low 3 bits are the running move's type. The client
    // (sub_6F02B0 -> +108 / +112, blob reader sub_499350) then walks a moving actor from pos1 to pos2
    // (event 30016) instead of placing it at the end of its path.
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    const uint32_t state_flags = motion_.state_flags(now);

    // ── header (sub_6F02B0 / server sub_715550) ──
    // ORDER (confirmed from both sides): entityId, KIND(=1 PC, actor+58), SPECIES(=type, actor+448),
    // hair, job. (The 2nd/3rd u16 are kind then species — easy to swap; getting kind wrong makes the
    // factory build the wrong/invisible actor class.) Retail sub_715550 writes +436 (the shown
    // model), +440 (its hair, u8) and +1024 (the class, u8); the client factory sub_6EA900 builds the
    // avatar from {28, type, hair, job, -1} — the same record Build_Local_Avatar fills for us.
    p.write<uint16_t>(eid);                            // entityId
    p.write<uint16_t>(1);                              // kind = 1 (Player/Avatar factory class)
    p.write<uint16_t>(static_cast<uint16_t>(type_));   // species/type (actor+448)
    p.write<uint8_t>(shown_hair());                    // hair (+440; client vtbl+164)
    p.write<uint8_t>(static_cast<uint8_t>(job_));      // job (+1024)
    // LEVEL (client sub_46F5A0 -> actor+920 = the level: Apply_33byte_StatsBlock stores the level
    // at the same +920; retail sub_715550 writes +1020 = level here). It was 0, so an appear sent
    // to ourselves (the op-9 warp-in on a re-enter) reset our own level display to 0.
    p.write<uint16_t>(static_cast<uint16_t>(level_));
    p.write<uint32_t>(state_flags);                    // state flags (sub_4989C0 -> actor+92): move type bits
    p.write<uint16_t>(static_cast<uint16_t>(cx)); p.write<uint16_t>(static_cast<uint16_t>(cy));          // pos1 = current
    p.write<uint16_t>(static_cast<uint16_t>(pos_x_)); p.write<uint16_t>(static_cast<uint16_t>(pos_y_));  // pos2 = destination
    p.write_bytes(char_name_, std::strlen(char_name_) + 1);  // name (NUL-terminated)

    // ── appearance blob (sub_6CDF30, minimal) ──
    // sub_49D5F0 base: sub_499350(actionState, facingDir, MOVE-SPEED = 3×u8) + u32 + sub_55FC50
    // (master=0 -> 1 byte) + u32. CONFIRMED byte-exact from the client reader sub_499350:
    //   b1 -> actionState (vtbl+128), b2 -> facingDir (this[29]=+116, b2*2pi/256),
    //   b3 -> MOVE SPEED: this[30] = (float)(4 * b3)  ==  actor+120, the per-tick step speed.
    // The move step advances dt_ms * (actor+120) * 0.001 tiles/frame (client sub_407180/sub_403380),
    // so b3=0 -> speed 0 -> the remote plays the walk animation but NEVER translates (= "walks in
    // place"). The ONLY writers of actor+120 are this appear byte and the subtype-472 speed command;
    // the appear is where a newly-seen player gets its speed. 4*b3 matches the client NPC walk
    // default (80 = 4*20); run would be ~140 (4*35). Observers now walk at this rate.
    p.write<uint8_t>(0);               // actionState
    p.write<uint8_t>(0);               // facingDir (updated by the first move)
    p.write<uint8_t>(move_speed_);     // MOVE SPEED -> client actor+120 = 4*move_speed_ (@speed sets it)
    // Character +452 (sub_7156D0 -> sub_60D230) -> the client's actor +1200 (the S2C 827 field): bit 0 = hidden.
    p.write<uint32_t>(hiding_ ? 1u : 0u);
    p.write<uint8_t>(0);   // equipment master = 0 (no worn items)
    p.write<uint32_t>(0);
    // bodyFlags = 0. bit0 ("hasModel") is NOT a good thing here: when set, the client THROWS AWAY
    // the species-built body and force-loads the fixed GM puppet chr100.nri (eagle+staff). With 0,
    // the client keeps the body built from the species (actor+448=type) -> the normal avatar.
    p.write<uint8_t>(0);
    // compactAppearance = Player +6208 (sub_73AB30): our species unless disguised — the client polymorphs
    // the body when it differs from the species (a disguise kit's model, item_use.cpp).
    p.write<uint32_t>(static_cast<uint32_t>(disguised_ ? shown_model_ : type_));
    // strMask: bit 8 = the party board's balloon [u8 type][str] (client sub_6C9450), bit 0x10 = the Mic's head
    // notice [str] (condition 8; client sub_6CDE70, megaphone.cpp); the other strings (1, 2, 4, 0x40) are not used.
    p.write<uint8_t>(static_cast<uint8_t>((board_type_ ? 0x08 : 0x00) | (mega_on_ ? 0x10 : 0x00)));
    if (board_type_) {
        p.write<uint8_t>(board_type_);
        p.write_bytes(board_text_.c_str(), board_text_.size() + 1);
    }
    if (mega_on_) p.write_bytes(mega_text_.c_str(), mega_text_.size() + 1);
    write_equip_appearance(p);   // sub_58CA90 / sub_6C8550: the worn items others see
    p.write<uint8_t>(0);   // discarded u8
    p.write<uint8_t>(0);   // str -> actor+15006 (empty)
    p.write<uint8_t>(0);   // str -> actor+14985 (empty)
    p.write<uint8_t>(0);   // actor+15031
    p.write<uint8_t>(0);   // actor+15032
    p.write<uint32_t>(0);  // actor+15036
    p.write<uint8_t>(0);   // actor+15040
    p.write<uint8_t>(0);   // actor+1208 (state; 0 -> not special)
    p.write<uint16_t>(static_cast<uint16_t>(shown_hair()));   // actor+15056 = hair
    p.write<uint16_t>(static_cast<uint16_t>(f_type_));  // actor+15058 = face/gender
    p.write<uint32_t>(0); p.write<uint32_t>(0); p.write<uint16_t>(0);  // sub_6CC910 (10 bytes)
    p.write<uint8_t>(0);   // actor+13772
    p.write<uint16_t>(0);  // sub_6C7CB0
    p.write<uint8_t>(0);   // actor+15760 (0 -> defaults to 1)
    return p;
}
