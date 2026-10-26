#include "game_session.hpp"
#include "game_server.hpp"
#include "client_text.hpp"
#include "logger.hpp"
#include <cstdlib>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  The Memory Port (Effect 34, Item_MemoryPortParam — items 7708 / 19659 / 19805 / 19810, db/memory_port_db.yml) —
//  1:1 with the retail GameServer (Player::OnProcessState sub_73D2A0 cases 817 / 820 / 823 @0x747FAB.., read from
//  its disassembly; UseItemHelper::Use case 0x22; hItemMemoryport* sub_789AB0 .. sub_789E00) and the Paula
//  procedures uspm_memory_port_select / _slot_insert / _slot_delete / _update_move (the Master's DB step, done here
//  at once). The client verified (Trickster.bin sub_6F0960 cases 818 .. 825, the record reader sub_6EE290).
//  A character keeps, per slot list type (0 = 5 slots, 1 = 10), saved points {slot, zone, point, description}.
//  The slot record (34 bytes, sub_60EBC0 / client sub_6EE290): [u16 slot][u32 zone][u16 x][u16 y][24 desc].
//   C2S 817 OPEN [u64 item uid] (under 8 bytes: the log, S2C 819 [u32 -1]); the item's row (sub_789AB0: in the bag,
//     of Item_MemoryPortParam — else the log, 819 [u32 -1]) -> its type's points -> S2C 818 [u64 uid][u8 n]{record}
//     (client: the item's window, sub_982CA0); a DB failure -> 819 [u32 code] (client "msg_cant_open").
//   C2S 820 ADD [u64 uid][u16 slot][str description] (under 10 bytes / no row: the log, S2C 822); a zone of
//     MapInfoEx Option 0x10 -> "invalid zone", 822; else the point here (our zone, our current point, 23 chars of
//     the text) — a slot already used -> 102 -> 822 (client "msg_cant_add") — -> S2C 821 [record] (client: the
//     slot shown, "eft_item_memoryport" + "etc_warp"). (No check of the slot against SlotNum: retail.)
//   C2S 823 DELETE [u64 uid][u16 slot]: no row -> the log, S2C 825; the point gone (not one row: 101 -> 825,
//     "msg_cant_delete") -> S2C 824 [u16 slot].
//   Use (Effect 34, custom data [u16 slot]): a zone of Option 128 keeps us (S2C 799 [u32 4], "TeleportCancelType4",
//     0); under 2 bytes -> 0; hItemMemoryport::SendUse (sub_6E51A0): the item and its row (else the log only),
//     one of it held back, the Master's update_move: the slot's point (none: 104 -> OnUseCancel, the log only),
//     the item used up -> OnUseOk (sub_789CE0): S2C 0x59 [u64 uid] (sub_5F66C0), the item's record, the
//     destination, the zone change asked for 200 ms later (changeZoneType 3, sub_50D810) and 2000 ms of protection
//     (sub_73CF80); the use itself answers 2.
//  (The client's 818 array holds 20 records: more are not sent — a slot past the list's size can only come from a
//  forged ADD.)
// ─────────────────────────────────────────────────────────────────────────────

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

void write_slot(PacketBuffer& p, uint16_t slot, int32_t zone, int32_t x, int32_t y, const std::string& desc_cp949)
{
    p.write<uint16_t>(slot);
    p.write<uint32_t>(static_cast<uint32_t>(zone));
    p.write<uint16_t>(static_cast<uint16_t>(x));
    p.write<uint16_t>(static_cast<uint16_t>(y));
    char d[24] = {};
    std::strncpy(d, desc_cp949.c_str(), 23);
    p.write_bytes(reinterpret_cast<const uint8_t*>(d), sizeof(d));
}

size_t readable(const PacketBuffer& pkt) { return pkt.size() > pkt.rpos() ? pkt.size() - pkt.rpos() : 0; }
} // namespace

// sub_789AB0: the row of the Memory Port item `uid` in our bag (+1716), or null.
const MemoryPortDb::Info* GameClientSession::memory_port_info(uint64_t uid)
{
    BagItem* b = bag_find_uid(uid);
    if (!b) {
        LOG_INFO("%s : hItemMemoryport::GetInfo() - itemUID : %llu", char_name_, static_cast<unsigned long long>(uid));
        return nullptr;
    }
    return server_.memory_port_db().get(b->item_id);
}

void GameClientSession::handle_memory_port_open(PacketBuffer& pkt)
{
    auto fail = [&](int32_t code) {                                       // S2C 819 (sub_603390)
        PacketBuffer p(static_cast<Opcode>(0x0333), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(code));
        send(p);
    };
    if (readable(pkt) < 8) {
        LOG_INFO("%s : CMD_MEMORY_PORT_OPEN - msg readable size invalid", char_name_);
        fail(-1);
        return;
    }
    const uint64_t uid = pkt.read<uint64_t>();
    const MemoryPortDb::Info* info = memory_port_info(uid);
    if (!info) {
        LOG_INFO("%s : CMD_MEMORY_PORT_OPEN - invalid memoryport info, ItemUID : %llu", char_name_,
                 static_cast<unsigned long long>(uid));
        fail(-1);
        return;
    }
    DBResult rows;
    try {                                                                 // uspm_memory_port_select
        rows = server_.db().query("SELECT slot_num, zone, pos_x, pos_y, pt_desc FROM memory_port "
                                  "WHERE char_uid = %u AND type = %d ORDER BY slot_num", char_uid_, info->type);
    } catch (const DBException& e) {
        LOG_ERROR("uspm_memory_port_select %u: %s", char_uid_, e.what());
        fail(1);
        return;
    }
    const size_t n = std::min<size_t>(rows.size(), 20);
    PacketBuffer p(static_cast<Opcode>(0x0332), is_paula());              // S2C 818 (sub_6032D0)
    p.write<uint64_t>(uid);
    p.write<uint8_t>(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const auto& r = rows[i];
        write_slot(p, static_cast<uint16_t>(std::atoi(r.at("slot_num").c_str())), std::atoi(r.at("zone").c_str()),
                   std::atoi(r.at("pos_x").c_str()), std::atoi(r.at("pos_y").c_str()), to_client_text(r.at("pt_desc")));
    }
    send(p);
    LOG_DEBUG("GameClientSession %u: Memory Port %llu opened (type %d, %zu point(s))", session_id(),
              static_cast<unsigned long long>(uid), info->type, n);
}

void GameClientSession::handle_memory_port_add(PacketBuffer& pkt)
{
    auto fail = [&]() {                                                   // S2C 822 (__fputchar_121)
        PacketBuffer p(static_cast<Opcode>(0x0336), is_paula());
        send(p);
    };
    if (readable(pkt) < 10) {
        LOG_INFO("%s : CMD_MEMORY_PORT_ADD - msg readable size invalid", char_name_);
        fail();
        return;
    }
    const uint64_t uid  = pkt.read<uint64_t>();
    const uint16_t slot = pkt.read<uint16_t>();
    const std::string desc = read_cstr(pkt).substr(0, 23);
    const MemoryPortDb::Info* info = memory_port_info(uid);
    if (!info) {
        LOG_INFO("%s : CMD_MEMORY_PORT_ADD - invalid memoryport info, ItemUID : %llu", char_name_,
                 static_cast<unsigned long long>(uid));
        fail();
        return;
    }
    // Hardening (not retail): the client's list holds 20 points; any u16 slot was a new row (up to 65,536 a list).
    if (slot > 20) {
        LOG_INFO("%s : CMD_MEMORY_PORT_ADD - invalid slot : %u", char_name_, static_cast<unsigned>(slot));
        fail();
        return;
    }
    if (const MapDef* z = server_.maps_db().get(zone_id_)) {
        if (z->option & 0x10) {
            LOG_INFO("%s : CMD_MEMORY_PORT_ADD - invalid zone, zoneID : %d", char_name_, zone_id_);
            fail();
            return;
        }
    }
    int32_t x, y;
    cur_pos(std::chrono::steady_clock::now(), x, y);
    int err = 0;
    try {                                                                 // uspm_memory_port_slot_insert
        DBTransaction tx(server_.db());
        const auto have = server_.db().query("SELECT slot_num FROM memory_port WHERE char_uid = %u AND type = %d "
                                             "AND slot_num = %u", char_uid_, info->type, static_cast<unsigned>(slot));
        if (!have.empty()) {
            err = 102;
        } else if (server_.db().execute("INSERT INTO memory_port (char_uid, type, slot_num, zone, pos_x, pos_y, "
                                        "pt_desc) VALUES (%u, %d, %u, %d, %d, %d, '%s')", char_uid_, info->type,
                                        static_cast<unsigned>(slot), static_cast<int>(zone_id_), x, y,
                                        server_.db().escape(from_client_text(desc)).c_str()) != 1) {
            err = 101;
        }
        if (!err) tx.commit();
    } catch (const DBException& e) {
        LOG_ERROR("uspm_memory_port_slot_insert %u: %s", char_uid_, e.what());
        err = 101;
    }
    if (err) {                                                            // hItemMemoryportAdd::OnCancel
        LOG_INFO("%s : hItemMemoryportAdd::OnCancel() - type : %d, index : %u", char_name_, info->type,
                 static_cast<unsigned>(slot));
        fail();
        return;
    }
    PacketBuffer p(static_cast<Opcode>(0x0335), is_paula());              // S2C 821 (sub_603400)
    write_slot(p, slot, zone_id_, x, y, desc);
    send(p);
    LOG_DEBUG("GameClientSession %u: Memory Port slot %u = zone %d (%d,%d)", session_id(), static_cast<unsigned>(slot),
              zone_id_, x, y);
}

void GameClientSession::handle_memory_port_delete(PacketBuffer& pkt)
{
    auto fail = [&]() {                                                   // S2C 825 (__fputchar_122)
        PacketBuffer p(static_cast<Opcode>(0x0339), is_paula());
        send(p);
    };
    uint64_t uid = 0;
    uint16_t slot = 0;
    try {
        uid  = pkt.read<uint64_t>();
        slot = pkt.read<uint16_t>();
    } catch (...) { return; }
    const MemoryPortDb::Info* info = memory_port_info(uid);
    if (!info) {
        LOG_INFO("%s, CMD_MEMORY_PORT_DELETE - invalid memoryport info, ItemUID : %llu", char_name_,
                 static_cast<unsigned long long>(uid));
        fail();
        return;
    }
    uint64_t gone = 0;
    try {                                                                 // uspm_memory_port_slot_delete
        gone = server_.db().execute("DELETE FROM memory_port WHERE char_uid = %u AND type = %d AND slot_num = %u",
                                    char_uid_, info->type, static_cast<unsigned>(slot));
    } catch (const DBException& e) {
        LOG_ERROR("uspm_memory_port_slot_delete %u: %s", char_uid_, e.what());
    }
    if (gone != 1) {                                                      // hItemMemoryportDelete::OnCancel
        LOG_INFO("%s : hItemMemoryportDelete::OnCancel() - type : %d, index : %u", char_name_, info->type,
                 static_cast<unsigned>(slot));
        fail();
        return;
    }
    PacketBuffer p(static_cast<Opcode>(0x0338), is_paula());              // S2C 824 (sub_6034A0)
    p.write<uint16_t>(slot);
    send(p);
}

int GameClientSession::use_memory_port(BagItem& b, const ItemDef& d)
{
    if (zone_no_port(zone_id_)) {                                         // (LABEL_202: sub_6028C0(4))
        PacketBuffer p(static_cast<Opcode>(0x031F), is_paula());
        p.write<uint32_t>(4);
        send(p);
        return 0;
    }
    if (use_data_.left() < 2) return 0;
    const uint16_t slot = use_data_.u16();
    const MemoryPortDb::Info* info = server_.memory_port_db().get(b.item_id);
    if (!info) {
        LOG_INFO("%s : hItemMemoryport::SendUse() - invalid memoryport - itemUID : %llu", char_name_,
                 static_cast<unsigned long long>(b.uid));
        return 2;
    }
    DBResult rows;
    try {                                                                 // uspm_memory_port_update_move
        rows = server_.db().query("SELECT zone, pos_x, pos_y FROM memory_port WHERE char_uid = %u AND type = %d AND "
                                  "slot_num = %u", char_uid_, info->type, static_cast<unsigned>(slot));
    } catch (const DBException& e) {
        LOG_ERROR("uspm_memory_port_update_move %u: %s", char_uid_, e.what());
    }
    if (rows.empty()) {                                                   // 104 -> OnUseCancel: the log only
        LOG_INFO("%s : hItemMemoryport::OnUseCancel() - itemUID : %llu, reason : %d", char_name_,
                 static_cast<unsigned long long>(b.uid), 104);
        return 2;
    }
    const int zone = std::atoi(rows[0].at("zone").c_str());
    const int16_t x = static_cast<int16_t>(std::atoi(rows[0].at("pos_x").c_str()));
    const int16_t y = static_cast<int16_t>(std::atoi(rows[0].at("pos_y").c_str()));
    // OnUseOk (sub_789CE0)
    {
        PacketBuffer p(static_cast<Opcode>(0x0059), is_paula());          // sub_5F66C0
        p.write<uint64_t>(b.uid);
        send(p);
    }
    const BagRecord rec = bag_decrease(b, 1);
    if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
    else               server_.item_store().SetAmount(rec.uid, rec.count);
    bag_apply(rec);
    protect(2000);
    LOG_DEBUG("GameClientSession %u: Memory Port %u slot %u -> zone %d (%d,%d)", session_id(), d.id,
              static_cast<unsigned>(slot), zone, x, y);
    auto self = shared_from_this();
    schedule_delayed(std::chrono::milliseconds(200), [this, self, zone, x, y]() {
        if (dead_ || leaving_ || state_ != GameClientSessionState::InWorld) return;
        if (leave_field(zone)) begin_field_change(static_cast<int16_t>(zone), x, y, 3);
    });
    return 2;
}
