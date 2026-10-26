#include "game_session.hpp"
#include "game_server.hpp"
#include "item_wire.hpp"
#include "item_option.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  The player-to-player trade — retail GameServer "ItemExc" (the Player state machine sub_73D2A0, its state 6;
//  the zone's ItemExchangeMgr +0xB68; hItemExchange -> USPM_ItemTrade_User), verified against the KR client.
//
//  The request (the normal state):
//    C2S 94 [u16 player] (case 94): the player found in the zone, alive, not in zone 47 / 218 (else S2C 96); already
//      in state 6 -> "CMD_ITEMEXC_REQUEST - invalid exchanged state", sub_762100, state 0. Else both link (+0xC7C),
//      S2C 94 [u16 us][cstr our name] to it (sub_5F39D0; client: the request dialog — it answers 99 by itself when
//      trades are refused) and we enter state 6.
//    C2S 97 (yes) -> event 30025 to the asker; C2S 99 (no) -> 30028 to it, the link off, S2C 100 to us; C2S 100
//      (cancel) -> 30029 to the linked one, the link off, S2C 100.
//  State 6 (sub_750819), the asker first: 30025 from its linked one -> checks (S2C 96 + 30027 to both when either is
//    in an exchange already) -> the exchange (sub_84D3C0, ids by the zone; +0xA80), S2C 95 to it (client: the window,
//    state 13) and 30026 [u16 id] to the other: +0xA80, S2C 98 (the window), state 6.
//    C2S 101 [u8 flag][u32 gelders][u16 n<=6][n x {u64 uid, u32 id, u32 count}] -> ItemExchangeMgr::Update
//      (sub_84D440: 5 checked already, 6 not in the bag, 7 flag 1024 (no trade), 8 gelders out of range, 9 the other's
//      would pass 2100000000, 10 n not 1..6) -> S2C 101 to both (sub_5FB780: [our side][u16 our overweight][the other's
//      side][its rare items' extra][u16 its overweight][u16 result]); 8 / 9: the result to us, 0 to it; other errors:
//      the cancel (30029 to it, S2C 100, the exchange gone, state 0).
//    C2S 102 [u8] (checked, sub_84D650) -> S2C 103 [u8 ours][u8 we were first] and 102 [u8 its][u8 we were first]
//      to each side.
//    C2S 171 (the first checker: the final step) -> S2C 171 to us, 170 to it; C2S 172 (we agree) -> +110, S2C 175 to
//      it; C2S 173 (no) -> both +110 off, S2C 174 to both; C2S 177 -> both off, S2C 178 to both; C2S 176 (do it) ->
//      +110, and with the other's +110: hItemExchange::Send(player1 = the linked one, player2 = us) (else the
//      cancel); C2S 179 [cstr] -> S2C 179 to it. (The client: the first checker sends 171 then 172, the other 176 —
//      so player1 is the first checker.)
//    C2S 100 / 0x40 -> the cancel; C2S 0xEE (a skill) -> S2C 239 [19]. Every other command: not handled (ignored).
//  hItemExchange::Send (sub_774790), on player1: its sync flag 0xC4000000 -> refused; both sides set and checked,
//    player1 is the first checker, A != B; each offered item in its owner's bag with that id and count, not worn, no uid twice (else the
//    two are cut, sub_8AA290 — retail's dupe guard); the records (sub_50CEF0 -> sub_6DC330 per side, then the
//    sub_6DC840 pass: a whole item that changes owner is 5 / 6, a part split off a stack a new row); each bag's room
//    (sub_6DC730: types 2 / 4 / 6 + the items <= 300); the gelders after it <= 0x7FFFFFFE; the DB
//    (USPM_ItemTrade_User, ItemStore::TradeItems) with both sync flags up. ICMD_EXCHANGE_ACK_OK (sub_7768C0, on player1): the
//    flags off, ItemExchangeMgr::Exchange (sub_84D740: each bag takes its records) -> S2C 105 [u32 gelders][u16 n]
//    [n records] to player1 (sub_5FBB60), 30030 to player2 -> S2C 104 the same way (sub_5FBAB0); state 0.
//    A failure: 30029 to the other, both cut, S2C 100.
//  Here the two players run on two strands: our half is made on ours (our bag), the other's half, the cross-side pass,
//  the DB and its S2C 104 on its strand, then ours finishes (S2C 105). The exchange records live in trades_ (a mutex).
//  Not here: sub_6DC580's pass (it matches identical records across the sides), the trade logs (sub_775320 slog).
// ─────────────────────────────────────────────────────────────────────────────

std::mutex                                   GameClientSession::trade_mx_;
std::map<uint16_t, GameClientSession::TradeEx> GameClientSession::trades_;
uint16_t                                     GameClientSession::trade_next_ = 1;

namespace {
constexpr int     kTradeBagCap   = 300;           // the bag's capacity (sub_6DC730: this[20])
constexpr int32_t kTradeMoneyMax = 1000000000;    // sub_84D440
constexpr uint32_t kTradeMoneyCap = 0x7D2B7500;   // sub_84D440: the other's gelders + ours must stay below

// The item's flags (sub_6ECE20 / sub_6ECD80): ItemParam Options, or a rare item's option of type 13 (its third
// value holds the flags).
uint32_t item_flags(const GameClientSession* /*unused*/, const ItemDef* d, const std::vector<ItemOption>& opts,
                    uint8_t kind)
{
    uint32_t f = d ? static_cast<uint32_t>(d->options) : 0;
    if (kind == 2)
        for (const ItemOption& o : opts)
            if (o.type == 13) { f = static_cast<uint32_t>(o.extra); break; }
    return f;
}
}  // namespace

// ── The dispatcher ────────────────────────────────────────────────────────────
void GameClientSession::handle_trade(PacketBuffer& pkt, uint16_t op)
{
    try {
        // 94 / 97 / 99 / 100 are cases of the normal state only (state 0): sitting / resting, a shop, the exchange
        // shop, a compound, a skill master, the warehouse, a drill or a pick-up leave them unhandled.
        if (!trade_ && (rest_state_ || shop_ || exchange_ || compound_ || sshop_ || drill_.active || pick_.active))
            return;
        if (op == 94) {                                   // the request
            const uint16_t target = pkt.read<uint16_t>();
            GameClientSession* t = server_.player_session(target);
            if (!t || t == this || t->zone_id() != zone_id_ || !t->in_world()) {
                PacketBuffer p(static_cast<Opcode>(96), is_paula());
                send(p);
                return;
            }
            if (trade_) {
                LOG_INFO("CMD_ITEMEXC_REQUEST - invalid exchanged state, charName=%s, CHRUID=%u", char_name_, char_uid_);
                trade_reset();
                trade_leave();
                return;
            }
            if (zone_id_ == 47 || zone_id_ == 218 || dead_) {
                PacketBuffer p(static_cast<Opcode>(96), is_paula());
                send(p);
                return;
            }
            trade_opposite_ = target;
            const uint16_t me = static_cast<uint16_t>(object_id_);
            const std::string name = char_name_;
            server_.post_to_session(t->session_id(), [me, name](GameClientSession& s) { s.trade_got_request(me, name); });
            trade_ = true;                                // state 6 (waiting for the answer)
            LOG_DEBUG("GameClientSession %u: trade request -> player %u", session_id(), target);
            return;
        }
        if (!trade_) {                                    // the normal state's answers
            const uint16_t opp = trade_opposite_;
            GameClientSession* o = opp ? server_.player_session(opp) : nullptr;
            const uint16_t me = static_cast<uint16_t>(object_id_);
            if (op == 97) {
                if (o) server_.post_to_session(o->session_id(), [me](GameClientSession& s) { s.trade_join(me); });
                else { PacketBuffer p(static_cast<Opcode>(100), is_paula()); send(p); }
            } else if (op == 99 || op == 100) {
                const int ev = (op == 99) ? 28 : 29;
                if (o) server_.post_to_session(o->session_id(), [me, ev](GameClientSession& s) { s.trade_event(me, ev); });
                trade_opposite_ = 0;
                PacketBuffer p(static_cast<Opcode>(100), is_paula());
                send(p);
            }
            return;
        }
        // State 6
        switch (op) {
        case 100: trade_cancel(); return;
        case 101: trade_update(pkt); return;
        case 102: trade_check(pkt.read<uint8_t>() != 0); return;
        case 179: {                                       // the window's chat
            std::string text;
            while (!pkt.at_end()) { const char c = static_cast<char>(pkt.read<uint8_t>()); if (!c) break; text += c; }
            GameClientSession* o = trade_opposite_ ? server_.player_session(trade_opposite_) : nullptr;
            if (o) server_.post_to_session(o->session_id(), [text](GameClientSession& s) {
                PacketBuffer p(static_cast<Opcode>(179), s.is_paula());
                p.write_bytes(text.c_str(), text.size() + 1);
                s.send(p);
            });
            return;
        }
        default: break;
        }
        // 171 / 172 / 173 / 176 / 177: the final step, on the exchange's +110 flags
        std::unique_lock<std::mutex> lk(trade_mx_);
        auto it = (trade_id_ >= 0) ? trades_.find(static_cast<uint16_t>(trade_id_)) : trades_.end();
        if (it == trades_.end()) { lk.unlock(); trade_cancel(); return; }
        TradeEx& ex = it->second;
        if (ex.exec) return;                              // Hardening (not retail): the offers are fixed (trade_update)
        const int me = (ex.side[0].oid == static_cast<uint16_t>(object_id_)) ? 0 : 1;
        TradeSide& mine = ex.side[me];
        TradeSide& his  = ex.side[1 - me];
        const uint32_t his_sid = his.sid;
        auto to_him = [&](uint16_t opcode) {
            server_.post_to_session(his_sid, [opcode](GameClientSession& s) {
                PacketBuffer p(static_cast<Opcode>(opcode), s.is_paula());
                s.send(p);
            });
        };
        auto to_me = [&](uint16_t opcode) { PacketBuffer p(static_cast<Opcode>(opcode), is_paula()); send(p); };
        if (op == 171) {
            if (ex.first != me) { lk.unlock(); trade_cancel(); return; }
            to_me(171);
            to_him(170);
        } else if (op == 172) {
            mine.agreed = true;
            to_him(175);
        } else if (op == 173) {
            mine.agreed = his.agreed = false;
            to_me(174);
            to_him(174);
        } else if (op == 177) {
            if (!mine.agreed) { lk.unlock(); trade_cancel(); return; }
            mine.agreed = his.agreed = false;
            to_me(178);
            to_him(178);
        } else if (op == 176) {
            if (mine.agreed) return;                      // already sent
            mine.agreed = true;
            if (!his.agreed) { lk.unlock(); trade_cancel(); return; }
            lk.unlock();
            // hItemExchange::Send(player1 = the linked one, player2 = us) — the call at 0x751484 passes the linked
            // one first, and Send wants player1 to be the first checker: the client's first checker sends 171 +
            // 172, the other 176. Player1's half runs on its strand; a refusal there is our cancel.
            const uint16_t me_oid = static_cast<uint16_t>(object_id_);
            server_.post_to_session(his_sid, [me_oid](GameClientSession& s) { s.trade_send(me_oid); });
        }
    } catch (...) {}
}

// State 6 swallows every command it does not handle (the retail state's switch); a skill gets S2C 239 [19] and an
// action (0x40) ends the trade.
// Hardening (not retail): an allow-list — the port's deny-list let the bag / gelder commands it did not name (a drop,
// the post box, scrolls, the gun, ...) run mid-trade, and in the exchange's DB step (trade_sync_) the stale records
// then overwrote what they changed (a dupe). What passes touches neither the bag nor the gelders (the session, chat,
// the party; a drop is refused by its handler, S2C 0x45); in the DB step not even the trade's own commands.
bool GameClientSession::trade_blocks(PacketBuffer& /*pkt*/, uint16_t op)
{
    if (!trade_) return false;
    switch (op) {
    // the trade: 94 ask, 97 yes, 99 no, 100 cancel, 101 the offer, 102 checked, 171 / 172 / 173 / 176 / 177 the final
    // step (179, the window's chat, below)
    case 0x005E: case 0x0061: case 0x0063: case 0x0064: case 0x0065: case 0x0066:
    case 0x00AB: case 0x00AC: case 0x00AD: case 0x00B0: case 0x00B1:
        return trade_sync_;
    case 0x00EE: {
        PacketBuffer p(static_cast<Opcode>(0x00EF), is_paula());   // S2C 239 [u16 19] (sub_5ED7D0)
        p.write<uint16_t>(19);
        send(p);
        return true;
    }
    case 0x0040:
        if (!trade_sync_) trade_cancel();
        return true;
    // the session, the zone / field steps, the heartbeats, the guide, the community registrations
    case 0x0004: case 0x0005: case 0x04A5: case 0x1581: case 0x1584: case 0x076C: case 0x076D: case 0x009C:
    case 0x2CD4: case 0x2CD7: case 0x2C89: case 0x2C8C: case 0x2C95: case 0x2C98:
    case 0x1771: case 0x2135: case 0x2712: case 0x2AF9: case 0x2B5E: case 0x2E7D:
    // chat (54 / 417 / 713) and the trade window's (179)
    case 0x0036: case 0x01A1: case 0x02C9: case 0x00B3:
    // the party and its board
    case 0x1B59: case 0x1B65: case 0x1B69: case 0x1B6C: case 0x1B6D: case 0x1B71: case 0x1B62: case 0x1B82:
    case 0x1B5C: case 0x1B5F: case 0x1B8E: case 0x1BA7: case 0x1B87: case 0x1B9A: case 0x1BA5: case 0x1BA8:
    case 0x1B9D: case 0x1BA0: case 0x1BA3: case 0x1BAB:
    case 0x1C86: case 0x1C89: case 0x1C8C: case 0x1C8F: case 0x1C92: case 0x1C95: case 0x1C98: case 0x1C99:
    case 0x1C9A: case 0x1C9D:
    // the post box's list / detail (read only), the stat points, a skill's level, the disguise's end
    case 1230: case 1233: case 0x006C: case 0x01E1: case 0x0401:
    // the second-password wrapper (its inner command comes back through here) and the drop it carries
    case 0x039F: case 0x0043:
        return false;
    default:
        return true;
    }
}

// S2C 94 to us (sub_5F39D0) and our link to the asker (sub_76EC90).
void GameClientSession::trade_got_request(uint16_t from, const std::string& name)
{
    if (!in_world()) return;
    trade_opposite_ = from;
    PacketBuffer p(static_cast<Opcode>(94), is_paula());
    p.write<uint16_t>(from);
    p.write_bytes(name.c_str(), name.size() + 1);
    send(p);
}

// Event 30025 on the asker (state 6): the asked one said yes.
void GameClientSession::trade_join(uint16_t from)
{
    if (!trade_ || from != trade_opposite_) return;
    GameClientSession* o = server_.player_session(from);
    if (!o) { trade_cancel(); return; }
    const uint32_t osid = o->session_id();
    std::unique_lock<std::mutex> lk(trade_mx_);
    bool other_busy = false;
    for (const auto& [id, ex] : trades_)
        if (ex.side[0].oid == from || ex.side[1].oid == from) other_busy = true;
    if (trade_id_ != -1 || other_busy) {
        lk.unlock();
        PacketBuffer p(static_cast<Opcode>(96), is_paula());
        send(p);
        LOG_INFO("OCMD_ITEMEXC_JOIN - %s is already exchanged state, chrName=%s, CHRUID=%u",
                 trade_id_ != -1 ? "Player" : "Opposite Player", char_name_, char_uid_);
        const uint16_t me = static_cast<uint16_t>(object_id_);
        trade_event(me, 27);
        server_.post_to_session(osid, [me](GameClientSession& s) { s.trade_event(me, 27); });
        return;
    }
    if (dead_) return;
    uint16_t id = trade_next_;
    while (trades_.count(id) || id == 0) ++id;
    trade_next_ = static_cast<uint16_t>(id + 1);
    TradeEx& ex = trades_[id];
    ex.id = id;
    ex.side[0].sid = session_id(); ex.side[0].oid = static_cast<uint16_t>(object_id_); ex.side[0].char_uid = char_uid_;
    ex.side[0].weight = bag_weight_; ex.side[0].max_wt = st_maxwt_; ex.side[0].purse = money_;
    ex.side[1].sid = osid;         ex.side[1].oid = from;
    lk.unlock();
    trade_id_ = id;
    PacketBuffer p(static_cast<Opcode>(95), is_paula());     // sub_5F39B0: the window
    send(p);
    const uint16_t me = static_cast<uint16_t>(object_id_);
    server_.post_to_session(osid, [me, id](GameClientSession& s) { s.trade_joined(me, id); });
    LOG_DEBUG("GameClientSession %u: trade %u opened with player %u", session_id(), id, from);
}

// Event 30026 on the asked one: +0xA80, S2C 98 (the window), state 6.
void GameClientSession::trade_joined(uint16_t from, uint16_t id)
{
    if (from != trade_opposite_ || !in_world()) return;
    {
        std::lock_guard<std::mutex> lk(trade_mx_);
        auto it = trades_.find(id);
        if (it == trades_.end()) return;
        TradeSide& s = it->second.side[1];
        s.char_uid = char_uid_;
        s.weight = bag_weight_; s.max_wt = st_maxwt_; s.purse = money_;
    }
    trade_id_ = id;
    PacketBuffer p(static_cast<Opcode>(98), is_paula());
    send(p);
    trade_ = true;
}

// Events 30027 (the join failed: sub_762100, state 0), 30028 (the asked one said no) and 30029 (cancelled).
void GameClientSession::trade_event(uint16_t from, int ev)
{
    if (ev == 27) {
        trade_reset();
        if (!dead_) trade_leave();
        return;
    }
    if (from != trade_opposite_) return;
    PacketBuffer p(static_cast<Opcode>(100), is_paula());
    send(p);
    if (!trade_) {                                       // the normal state: the link off
        trade_opposite_ = 0;
        return;
    }
    if (ev == 28 && trade_id_ >= 0) {
        std::lock_guard<std::mutex> lk(trade_mx_);
        trades_.erase(static_cast<uint16_t>(trade_id_));
    }
    trade_leave();
}

// Out of state 6 (its leave clears +0xC7C); the exchange id goes with it.
void GameClientSession::trade_leave()
{
    trade_          = false;
    trade_opposite_ = 0;
    trade_id_       = -1;
    trade_sync_     = false;
}

// The state-6 cancel (C2S 100, a failed update / step): 30029 to the other, S2C 100, the exchange gone, state 0.
void GameClientSession::trade_cancel()
{
    const uint16_t me = static_cast<uint16_t>(object_id_);
    if (GameClientSession* o = trade_opposite_ ? server_.player_session(trade_opposite_) : nullptr)
        server_.post_to_session(o->session_id(), [me](GameClientSession& s) { s.trade_event(me, 29); });
    PacketBuffer p(static_cast<Opcode>(100), is_paula());
    send(p);
    if (trade_id_ >= 0) {
        std::lock_guard<std::mutex> lk(trade_mx_);
        trades_.erase(static_cast<uint16_t>(trade_id_));
    }
    trade_leave();
}

// sub_762100 (a new request while trading, a failed join, our leaving): 30029 to both sides of our exchange and
// to the linked one, the exchange gone, S2C 100 to us.
void GameClientSession::trade_reset()
{
    const uint16_t me = static_cast<uint16_t>(object_id_);
    std::vector<uint32_t> tell;
    if (trade_id_ >= 0) {
        std::lock_guard<std::mutex> lk(trade_mx_);
        auto it = trades_.find(static_cast<uint16_t>(trade_id_));
        if (it != trades_.end()) {
            for (const TradeSide& s : it->second.side)
                if (s.sid && s.oid != me) tell.push_back(s.sid);
            trades_.erase(it);
        }
        trade_id_ = -1;
    }
    if (GameClientSession* o = trade_opposite_ ? server_.player_session(trade_opposite_) : nullptr) {
        if (std::find(tell.begin(), tell.end(), o->session_id()) == tell.end()) tell.push_back(o->session_id());
        trade_opposite_ = 0;
    }
    for (uint32_t sid : tell) server_.post_to_session(sid, [me](GameClientSession& s) { s.trade_event(me, 29); });
    PacketBuffer p(static_cast<Opcode>(100), is_paula());
    send(p);
}

// sub_73B970: the weight after the exchange above MaxWT (0 when under) — ours less what we give plus what we get.
int GameClientSession::trade_over_weight(const TradeSide& me, const TradeSide& other) const
{
    auto w = [&](const TradeSide& s) {
        int32_t sum = 0;
        for (const TradeSlot& t : s.slot) {
            if (!t.set()) continue;
            const ItemDef* d = server_.item_db().get(t.item_id);
            if (d) sum += d->weight * d->item_count(static_cast<uint16_t>(t.count));
        }
        return sum;
    };
    const int32_t after = me.weight - w(me) + w(other);
    return (after > me.max_wt) ? after - me.max_wt : 0;
}

// S2C 101 (sub_5FB780) as side `me` sees it: [u8 flag][u32 gelders][u16 6] + 6 x ([u8 kind][item data, a common item
// with the offered count] | [u8 0]) for ours, [u16 our overweight], the same for the other's, [u16 n] + n x ([u64 uid]
// + the extra info) of the other's rare items, [u16 its overweight][u16 result]. (VERIFIED: client sub_6F0960 case 101
// reads both sides with sub_6ED620 — n <= 6 items by kind —, sub_592320 the rare items' [u16 n]{u64, [u8 n]{12}}, then
// two u16; result 8 / 9 shows Word_Trader_MoneyOver / Word_OppTrader_MoneyOver.)
void GameClientSession::trade_view(const TradeEx& ex, int me, uint16_t my_over, uint16_t other_over, uint16_t result,
                                   PacketBuffer& p) const
{
    auto side = [&](const TradeSide& s, std::vector<const TradeSlot*>* rares) {
        p.write<uint8_t>(s.flag ? 1 : 0);
        p.write<uint32_t>(static_cast<uint32_t>(s.money));
        p.write<uint16_t>(6);
        for (const TradeSlot& t : s.slot) {
            if (!t.set() || !t.item.item_id) { p.write<uint8_t>(0); continue; }
            const uint16_t cnt = (t.item.kind == 1) ? static_cast<uint16_t>(t.count) : t.item.count;
            p.write<uint8_t>(t.item.kind);
            write_item_data(p, t.item.kind, t.item.item_id, t.item.uid, cnt, t.item.rare);
            if (rares && t.item.kind == 2) rares->push_back(&t);
        }
    };
    const TradeSide& a = ex.side[me];
    const TradeSide& b = ex.side[1 - me];
    side(a, nullptr);
    p.write<uint16_t>(my_over);
    std::vector<const TradeSlot*> rares;
    side(b, &rares);
    p.write<uint16_t>(static_cast<uint16_t>(rares.size()));
    for (const TradeSlot* t : rares) {
        p.write<uint64_t>(t->item.uid);
        write_rare_extra(p, server_.item_db().get(t->item.item_id), t->item.opts);
    }
    p.write<uint16_t>(other_over);
    p.write<uint16_t>(result);
}

// C2S 101: ItemExchangeMgr::Update (sub_84D440) on our side, then S2C 101 to both.
void GameClientSession::trade_update(PacketBuffer& pkt)
{
    const bool    flag  = pkt.read<uint8_t>() != 0;
    const int32_t money = static_cast<int32_t>(pkt.read<uint32_t>());
    int n = pkt.read<uint16_t>();
    if (n > 6) n = 6;
    TradeSlot in[6];
    for (int i = 0; i < n; ++i) {
        in[i].uid     = pkt.read<uint64_t>();
        in[i].item_id = pkt.read<uint32_t>();
        in[i].count   = static_cast<int32_t>(pkt.read<uint32_t>());
    }
    std::unique_lock<std::mutex> lk(trade_mx_);
    auto it = (trade_id_ >= 0) ? trades_.find(static_cast<uint16_t>(trade_id_)) : trades_.end();
    if (it == trades_.end()) { lk.unlock(); trade_cancel(); return; }
    TradeEx& ex = it->second;
    // Hardening (not retail): once hItemExchange::Send runs (trade_execute made the first checker's records from these
    // offers) neither side may change them — an uncheck + a new offer before the other's half ran made the two halves
    // of the DB step disagree. The command is ignored (so are 102 and the final step's).
    if (ex.exec) return;
    const int me = (ex.side[0].oid == static_cast<uint16_t>(object_id_)) ? 0 : 1;
    TradeSide& mine = ex.side[me];
    TradeSide& his  = ex.side[1 - me];
    mine.weight = bag_weight_; mine.max_wt = st_maxwt_; mine.purse = money_;
    int result = 0;
    if (n <= 0) {
        result = 10;
    } else if (mine.checked) {
        LOG_INFO("ItemExchangeMgr::Update - %s - piep->bChecked is true", char_name_);
        result = 5;
    } else {
        // inventory.HasAll_MatchUidAndId (sub_6DB270 -> sub_6DCCE0): the uid in the bag, that id, enough of it
        for (int i = 0; i < n && !result; ++i) {
            if (!in[i].set()) continue;
            const BagItem* b = bag_find_uid(in[i].uid);
            const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
            if (!b || !d || b->item_id != in[i].item_id || in[i].count <= 0 ||
                d->item_count(b->count) < in[i].count) {
                LOG_INFO("ItemExchangeMgr::Update - %s - inventory.HasAll_MatchUidAndId() returns false", char_name_);
                result = 6;
                break;
            }
            in[i].item = *b;
            // IsExchangeable (sub_84D5D0): the item's flag 1024
            if (item_flags(this, d, b->opts, b->kind) & 1024) {
                LOG_INFO("ItemExchangeMgr::Update - %s - IsExchangeable returns false", char_name_);
                result = 7;
            }
        }
        if (!result) {
            if (money < 0 || money > money_ || money > kTradeMoneyMax) {
                LOG_INFO("ItemExchangeMgr::Update - %s - money is invalid:%d", char_name_, money);
                mine.money = 0;
                mine.flag  = false;
                result = 8;
            } else if (static_cast<uint32_t>(money) + static_cast<uint32_t>(his.purse) >= kTradeMoneyCap) {
                mine.money = 0;
                mine.flag  = false;
                result = 9;
            } else {
                for (int i = 0; i < 6; ++i) mine.slot[i] = (i < n) ? in[i] : TradeSlot{};   // sub_84DAC0
                mine.money = money;                                                  // sub_84DB50
                mine.flag  = flag;
            }
        }
    }
    if (result != 0 && result != 8 && result != 9) { lk.unlock(); trade_cancel(); return; }
    const uint16_t my_over  = static_cast<uint16_t>(trade_over_weight(mine, his));   // sub_75C5E0
    const uint16_t his_over = static_cast<uint16_t>(trade_over_weight(his, mine));
    PacketBuffer mp(static_cast<Opcode>(101), is_paula());
    trade_view(ex, me, my_over, his_over, static_cast<uint16_t>(result), mp);
    PacketBuffer hp(static_cast<Opcode>(101), is_paula());
    trade_view(ex, 1 - me, his_over, my_over, result ? 0 : 0, hp);
    const uint32_t his_sid = his.sid;
    lk.unlock();
    send(mp);
    server_.post_to_session(his_sid, [hp](GameClientSession& s) { PacketBuffer q = hp; s.send(q); });
}

// C2S 102 (sub_84D650): our checked flag — the first to check is remembered, unchecking clears both — then S2C 103
// [u8 ours][u8 we were first] and 102 [u8 its][u8 we were first] to each.
void GameClientSession::trade_check(bool on)
{
    std::unique_lock<std::mutex> lk(trade_mx_);
    auto it = (trade_id_ >= 0) ? trades_.find(static_cast<uint16_t>(trade_id_)) : trades_.end();
    if (it == trades_.end()) { lk.unlock(); trade_cancel(); return; }
    TradeEx& ex = it->second;
    if (ex.exec) return;                                  // Hardening (not retail): the offers are fixed (trade_update)
    const int me = (ex.side[0].oid == static_cast<uint16_t>(object_id_)) ? 0 : 1;
    TradeSide& mine = ex.side[me];
    TradeSide& his  = ex.side[1 - me];
    mine.checked = on;
    if (!on) his.checked = false;
    if (on && !his.checked)        ex.first = me;
    else if (!on && !his.checked)  ex.first = -1;
    const uint8_t my_first  = (ex.first == me) ? 1 : 0;
    const uint8_t his_first = (ex.first == 1 - me) ? 1 : 0;
    const uint8_t mc = mine.checked ? 1 : 0, hc = his.checked ? 1 : 0;
    const uint32_t his_sid = his.sid;
    lk.unlock();
    PacketBuffer a(static_cast<Opcode>(103), is_paula()); a.write<uint8_t>(mc); a.write<uint8_t>(my_first); send(a);
    PacketBuffer b(static_cast<Opcode>(102), is_paula()); b.write<uint8_t>(hc); b.write<uint8_t>(my_first); send(b);
    server_.post_to_session(his_sid, [hc, mc, his_first](GameClientSession& s) {
        PacketBuffer x(static_cast<Opcode>(103), s.is_paula()); x.write<uint8_t>(hc); x.write<uint8_t>(his_first); s.send(x);
        PacketBuffer y(static_cast<Opcode>(102), s.is_paula()); y.write<uint8_t>(mc); y.write<uint8_t>(his_first); s.send(y);
    });
}

// sub_6DC330 for one side: the records of `me`'s bag — what it receives first nets off what it gives of the same
// common item; the rest joins its bag (sub_6DB7C0: a common item onto the first stack of that id, type 1; else the
// giver's item with its uid, type 2); what it still gives leaves (sub_6DBF00: type 3, or type 1 with what stays).
void GameClientSession::trade_records(const TradeSide& me, const TradeSide& other, std::vector<BagRecord>& out)
{
    struct Give { uint64_t uid; uint32_t id; int32_t count; const BagItem* item; };
    std::vector<Give> give;
    for (const TradeSlot& t : me.slot)
        if (t.set()) give.push_back({ t.uid, t.item_id, t.count, &t.item });
    for (const TradeSlot& t : other.slot) {
        if (!t.set()) continue;
        const ItemDef* d = server_.item_db().get(t.item_id);
        int32_t cnt = t.count;
        if (d && d->stackable()) {
            for (Give& g : give) {
                if (g.id != t.item_id || g.count <= 0) continue;
                cnt -= g.count;
                if (cnt >= 0) g.count = 0;
                else          g.count = -cnt;
                break;
            }
        }
        if (cnt <= 0) continue;
        BagRecord r;
        r.kind    = t.item.kind;
        r.item_id = t.item_id;
        std::memcpy(r.rare, t.item.rare, 4);
        r.opts    = t.item.opts;
        const BagItem* stack = nullptr;
        if (d && d->stackable())
            for (const BagItem& b : bag_) if (b.item_id == t.item_id) { stack = &b; break; }
        if (stack) {
            r.type  = 1;
            r.uid   = stack->uid;
            r.count = static_cast<uint16_t>(stack->count + cnt);
        } else {
            r.type  = 2;
            r.uid   = t.uid;
            r.count = (r.kind == 1) ? static_cast<uint16_t>(cnt) : t.item.count;
        }
        out.push_back(r);
    }
    for (const Give& g : give) {
        if (g.count <= 0) continue;
        const BagItem* b = bag_find_uid(g.uid);
        if (!b) continue;
        out.push_back(bag_decrease(*b, static_cast<uint16_t>(g.count)));
    }
}

// The other's C2S 176 on us, the first checker (player1 of hItemExchange::Send): our half; a refusal is the 176
// sender's cancel (0x75149F: 30029 to us, S2C 100 to it, the exchange gone, state 0).
void GameClientSession::trade_send(uint16_t from)
{
    if (trade_ && from == trade_opposite_ && trade_execute() == 0) return;
    if (GameClientSession* o = server_.player_session(from))
        server_.post_to_session(o->session_id(), [](GameClientSession& s) { if (s.trade_) s.trade_cancel(); });
}

// hItemExchange::Send, player1's half (we are the first checker): the checks on our side, our records, our
// gelders; player2's half runs on its strand (trade_exec_other). Non-zero: refused (the caller cancels).
int GameClientSession::trade_execute()
{
    if (trade_sync_) {
        LOG_INFO("%s : hItemExchange::Send HasFlag( 0xC4000000 )", char_name_);
        return 1;
    }
    std::unique_lock<std::mutex> lk(trade_mx_);
    auto it = (trade_id_ >= 0) ? trades_.find(static_cast<uint16_t>(trade_id_)) : trades_.end();
    if (it == trades_.end()) return 1;
    TradeEx& ex = it->second;
    const int me = (ex.side[0].oid == static_cast<uint16_t>(object_id_)) ? 0 : 1;
    TradeSide& mine = ex.side[me];
    TradeSide& his  = ex.side[1 - me];
    if (!mine.oid || !his.oid || !mine.checked || !his.checked || mine.oid == his.oid || ex.first != me) return 1;
    // our items: in the bag with that id and count, not worn, no uid twice (sub_50CE80 / sub_6DA7F0)
    for (int i = 0; i < 6; ++i) {
        const TradeSlot& t = mine.slot[i];
        if (!t.set()) continue;
        const BagItem* b = bag_find_uid(t.uid);
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d || b->item_id != t.item_id || d->item_count(b->count) < t.count) {
            LOG_INFO("[ITEMEX]:hItemExchange::Send - mismatch iteminfo : %s, idx:%u, uid:%llu, cnt:%d", char_name_,
                     t.item_id, static_cast<unsigned long long>(t.uid), t.count);
            return 1;
        }
        if (is_equipped(t.uid)) {
            LOG_INFO("[ITEMEX]:hItemExchange::Send - Items is equiped.: %s", char_name_);
            return 1;
        }
        for (int j = i + 1; j < 6; ++j)
            if (mine.slot[j].set() && mine.slot[j].uid == t.uid) {
                LOG_INFO("[ITEMEX]:hItemExchange::Send - duplicated uid. player1:%s, itemUid:%llu", char_name_,
                         static_cast<unsigned long long>(t.uid));
                const uint32_t his_sid = his.sid;
                lk.unlock();
                server_.post_to_session(his_sid, [](GameClientSession& s) { s.close(); });   // sub_8AA290 x 2
                close();
                return 11;
            }
    }
    mine.recs.clear();
    trade_records(mine, his, mine.recs);
    int add = 0;
    for (const BagRecord& r : mine.recs) if (r.type == 2 || r.type == 4 || r.type == 6) ++add;
    if (add + static_cast<int>(bag_.size()) > kTradeBagCap) {
        LOG_INFO("hItemExchange: excess item slot: p1:%s calc.cnt:%d", char_name_, add);
        return 1;
    }
    const int64_t settled = static_cast<int64_t>(money_) + his.money - mine.money;
    if (settled < 0 || settled > 0x7FFFFFFE) {
        LOG_INFO("hItemExchange: settled money is under 0 — p1:%s, p1org:%d, settled:%lld", char_name_, money_,
                 static_cast<long long>(settled));
        return 1;
    }
    mine.settled = static_cast<int32_t>(settled);
    mine.purse   = money_;
    mine.before.clear();                                      // (re-checked by trade_exec_done)
    for (const BagItem& b : bag_) mine.before.emplace_back(b.uid, b.count);
    ex.exec = true;                                           // (the offers are fixed from here)
    const uint32_t his_sid = his.sid;
    const uint16_t id = ex.id;
    lk.unlock();
    trade_sync_ = true;                                       // sub_50D080(+1636, 0xC4000000)
    const uint16_t my_oid = static_cast<uint16_t>(object_id_);
    server_.post_to_session(his_sid, [my_oid, id](GameClientSession& s) { s.trade_exec_other(my_oid, id); });
    return 0;
}

// The other's half on its strand: its checks and records, the cross-side pass (sub_6DC840), the room, the DB
// (USPM_ItemTrade_User), its bag + S2C 104, then the executor finishes (trade_exec_done).
void GameClientSession::trade_exec_other(uint16_t from, uint16_t id)
{
    auto fail = [&](const char* why) {
        LOG_INFO("[ITEMEX] trade %u: %s (%s)", id, why, char_name_);
        GameClientSession* o = server_.player_session(from);
        if (o) server_.post_to_session(o->session_id(), [me = static_cast<uint16_t>(object_id_), id](GameClientSession& s) {
            s.trade_exec_done(me, id, false);
        });
        trade_cancel();
    };
    if (!trade_ || trade_id_ != id || from != trade_opposite_) { fail("not in that exchange"); return; }
    std::unique_lock<std::mutex> lk(trade_mx_);
    auto it = trades_.find(id);
    if (it == trades_.end()) { lk.unlock(); fail("exchange gone"); return; }
    TradeEx& ex = it->second;
    const int me = (ex.side[0].oid == static_cast<uint16_t>(object_id_)) ? 0 : 1;
    TradeSide& mine = ex.side[me];
    TradeSide& his  = ex.side[1 - me];
    for (int i = 0; i < 6; ++i) {
        const TradeSlot& t = mine.slot[i];
        if (!t.set()) continue;
        const BagItem* b = bag_find_uid(t.uid);
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !d || b->item_id != t.item_id || d->item_count(b->count) < t.count || is_equipped(t.uid)) {
            lk.unlock();
            fail("[ITEMEX]:hItemExchange::Send - mismatch opposite iteminfo");
            return;
        }
        for (int j = i + 1; j < 6; ++j)
            if (mine.slot[j].set() && mine.slot[j].uid == t.uid) {
                lk.unlock();
                LOG_INFO("[ITEMEX]:hItemExchange::Send - duplicated uid. player2:%s", char_name_);
                if (GameClientSession* o = server_.player_session(from))
                    server_.post_to_session(o->session_id(), [](GameClientSession& s) { s.close(); });
                close();
                return;
            }
    }
    mine.recs.clear();
    trade_records(mine, his, mine.recs);
    // sub_6DC840 both ways: a part split off the giver's stack is a new row (uid 0); a whole item that the other
    // takes with its uid is 5 (the giver) / 6 (the taker).
    auto pass = [](std::vector<BagRecord>& a, std::vector<BagRecord>& b) {
        for (BagRecord& r : a) {
            BagRecord* o = nullptr;
            for (BagRecord& x : b) if (x.uid == r.uid) { o = &x; break; }
            if (!o) continue;
            if (r.type == 2 && o->type == 1) r.uid = 0;
            else if (r.type == 3 && o->type == 2) { r.type = 5; o->type = 6; }
        }
    };
    pass(mine.recs, his.recs);
    pass(his.recs, mine.recs);
    int add = 0;
    for (const BagRecord& r : mine.recs) if (r.type == 2 || r.type == 4 || r.type == 6) ++add;
    if (add + static_cast<int>(bag_.size()) > kTradeBagCap) { lk.unlock(); fail("hItemExchange: excess item slot"); return; }
    const int64_t settled = static_cast<int64_t>(money_) + his.money - mine.money;
    if (settled < 0 || settled > 0x7FFFFFFE) { lk.unlock(); fail("hItemExchange: settled money is under 0"); return; }
    mine.settled = static_cast<int32_t>(settled);
    // the DB lists (records -> USPM_ItemTrade_User flags: 1 -> 2 update, 3 / 5 -> 0 delete, 2 / 4 / 6 -> 1 insert)
    auto rows = [](const std::vector<BagRecord>& recs) {
        std::vector<ItemStore::TradeRow> v;
        for (const BagRecord& r : recs) {
            ItemStore::TradeRow t;
            t.uid = r.uid; t.item_id = r.item_id; t.item_class = r.kind; t.count = r.count;
            t.flag = (r.type == 1) ? 2 : ((r.type == 3 || r.type == 5) ? 0 : 1);
            v.push_back(t);
        }
        return v;
    };
    std::vector<ItemStore::TradeRow> my_rows  = rows(mine.recs);
    std::vector<ItemStore::TradeRow> his_rows = rows(his.recs);
    const int32_t my_settled = mine.settled, his_settled = his.settled;
    const uint32_t my_char = mine.char_uid, his_char = his.char_uid;
    lk.unlock();
    trade_sync_ = true;
    const int err = server_.item_store().TradeItems(his_char, his_rows, his_settled, my_char, my_rows, my_settled);
    if (err != 0) {
        trade_sync_ = false;
        fail("USPM_ItemTrade_User failed");
        return;
    }
    // the new uids back into the records (a new common stack; a rare item keeps its own)
    {
        std::lock_guard<std::mutex> lk2(trade_mx_);
        auto it2 = trades_.find(id);
        if (it2 != trades_.end()) {
            TradeEx& e2 = it2->second;
            auto fix = [](std::vector<BagRecord>& recs, const std::vector<ItemStore::TradeRow>& rs) {
                for (size_t k = 0; k < recs.size() && k < rs.size(); ++k)
                    if (rs[k].new_uid) recs[k].uid = rs[k].new_uid;
            };
            fix(e2.side[me].recs, my_rows);
            fix(e2.side[1 - me].recs, his_rows);
            e2.committed = true;                             // (trade_close: the first checker's side is due)
            std::vector<BagRecord> recs = e2.side[me].recs;
            trade_sync_ = false;
            trade_apply(recs, my_settled, 104);              // 30030 -> S2C 104 (sub_5FBAB0)
        }
    }
    GameClientSession* o = server_.player_session(from);
    if (o) server_.post_to_session(o->session_id(), [me_oid = static_cast<uint16_t>(object_id_), id](GameClientSession& s) {
        s.trade_exec_done(me_oid, id, true);
    });
    trade_leave();
    LOG_INFO("GameClientSession %u: trade %u done (gelders %d)", session_id(), id, money_);
}

// ICMD_EXCHANGE_ACK_OK on the executor (sub_7768C0): the sync flag off; done -> our records + S2C 105, the exchange
// gone, state 0; failed -> S2C 100 and state 0 (the other cancelled already).
void GameClientSession::trade_exec_done(uint16_t from, uint16_t id, bool ok)
{
    trade_sync_ = false;
    if (!ok) {
        if (trade_id_ == id) {
            PacketBuffer p(static_cast<Opcode>(100), is_paula());
            send(p);
            std::lock_guard<std::mutex> lk(trade_mx_);
            trades_.erase(id);
            trade_leave();
        }
        return;
    }
    std::vector<BagRecord> recs;
    int32_t settled = money_, purse = money_;
    std::vector<std::pair<uint64_t, uint16_t>> before;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(trade_mx_);
        auto it = trades_.find(id);
        if (it != trades_.end()) {
            const int me = (it->second.side[0].oid == static_cast<uint16_t>(object_id_)) ? 0 : 1;
            recs    = it->second.side[me].recs;
            settled = it->second.side[me].settled;
            purse   = it->second.side[me].purse;
            before  = it->second.side[me].before;
            found   = true;
            trades_.erase(it);
        }
    }
    (void)from;
    // Hardening (not retail): our records were made from our bag in trade_execute and set counts as they were then (the
    // DB step wrote them so). If the bag or the gelders changed since (trade_blocks lets nothing do so), they are not
    // applied: the gelders follow the DB (plus what changed meanwhile) and the session closes to reload the bag from it.
    if (found) {
        bool same = (money_ == purse && before.size() == bag_.size());
        for (size_t i = 0; same && i < bag_.size(); ++i)
            same = (bag_[i].uid == before[i].first && bag_[i].count == before[i].second);
        if (!same) {
            const int64_t m = static_cast<int64_t>(settled) + money_ - purse;
            money_ = static_cast<int32_t>(std::min<int64_t>(std::max<int64_t>(m, 0), 0x7FFFFFFE));
            LOG_WARN("[ITEMEX] %s: the bag / gelders changed during trade %u's DB step - its records are not applied "
                     "(gelders %d), the session closes", char_name_, id, money_);
            trade_leave();
            save_position();
            close();
            return;
        }
    }
    trade_apply(recs, settled, 105);
    trade_leave();
    LOG_INFO("GameClientSession %u: trade %u done (gelders %d)", session_id(), id, money_);
}

// Hardening (not retail): our connection closes while our exchange's DB step runs (trade_sync_: we are the first
// checker, between trade_execute and trade_exec_done) — the logout save would write our gelders from before it over
// what the DB step wrote. Done already (the other's strand ran USPM_ItemTrade_User): our side applies now, so the save
// writes what the DB holds; not yet: the exchange goes, and the other's half finds it gone and cancels. (One io
// thread: the other's half cannot run in between.)
void GameClientSession::trade_close()
{
    if (!trade_sync_) return;
    std::vector<BagRecord> recs;
    int32_t settled = money_;
    bool done = false;
    {
        std::lock_guard<std::mutex> lk(trade_mx_);
        auto it = (trade_id_ >= 0) ? trades_.find(static_cast<uint16_t>(trade_id_)) : trades_.end();
        if (it != trades_.end()) {
            const int me = (it->second.side[0].oid == static_cast<uint16_t>(object_id_)) ? 0 : 1;
            done    = it->second.committed;
            recs    = it->second.side[me].recs;
            settled = it->second.side[me].settled;
            trades_.erase(it);
        }
    }
    const int32_t id = trade_id_;
    trade_sync_ = false;
    if (done) trade_apply(recs, settled, 105);
    trade_leave();
    LOG_INFO("[ITEMEX] %s: connection closed in trade %d's DB step - %s (gelders %d)", char_name_, id,
             done ? "settled" : "cancelled", money_);
}

// ItemExchangeMgr::Exchange (sub_84D740) on our bag: 1 sets a count, 3 / 5 take the item out, 2 / 4 / 6 put one in
// (its uid from the DB); our gelders; then S2C 104 / 105 [u32 gelders][u16 n][n records] (sub_5FBAB0 / sub_5FBB60 —
// VERIFIED: client cases 104 / 105 read the gelders, n <= 12 and the bag records).
void GameClientSession::trade_apply(const std::vector<BagRecord>& recs, int32_t money, uint16_t opcode)
{
    for (const BagRecord& r : recs) {
        if (r.type == 1 || r.type == 3 || r.type == 5) {
            bag_apply(r);
            continue;
        }
        BagItem b;
        b.uid     = r.uid;
        b.item_id = r.item_id;
        b.count   = r.count;
        b.kind    = r.kind;
        std::memcpy(b.rare, r.rare, 4);
        b.opts    = r.opts;
        bag_.push_back(b);
        if (const ItemDef* d = server_.item_db().get(r.item_id)) bag_weight_ += d->weight * d->item_count(r.count);
    }
    money_ = money;
    PacketBuffer p(static_cast<Opcode>(opcode), is_paula());
    p.write<uint32_t>(static_cast<uint32_t>(money_));
    p.write<uint16_t>(static_cast<uint16_t>(recs.size()));
    for (const BagRecord& r : recs) write_bag_record(p, r);
    send(p);
    recompute_stats();
}
