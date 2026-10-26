#include "game_session.hpp"
#include "game_server.hpp"
#include "client_text.hpp"
#include "combat_util.hpp"
#include "logger.hpp"
#include <cstdio>
#include <cstdlib>

// ─────────────────────────────────────────────────────────────────────────────
//  Pets — retail GameServer (class Pet, object kind 4), verified against the KR client.
//
//  A pet item (ItemParam Type 13, equipment slot 8) is handled by the PetItemHelper (vtable 0x9AD68C):
//    OnLoad    (+12 sub_6E6B10) — the equipment's OnLoad (sub_75DEA0): at the login (sub_7FD820, after the
//              sector enter and our op 7) and at a zone enter (sub_40C0F0, before EnterZone): the pet is
//              made and shown with op 8 to its area (sub_5F6F40).
//    OnEquip   (+16 sub_6E6C30) — EquipNReleaseItem's slot set (sub_763B10), before S2C 0x52/0xEC: the pet
//              is made and, unless we are dead (+92 flag 4), shown with op 9 (sub_5F6FC0: the warp-in).
//    OnRelease (+20 sub_6E6DF0) — before S2C 0x55/0xED: the pet's owner is cleared (sub_6E6E60) and it
//              leaves (vtbl+28 sub_715530 -> Shell::DestroyObject case 4 -> its OnLeave sub_7381F0:
//              S2C 0x000F [u16 id][u8 0] to the area, sub_5F7610).
//  Both OnLoad and OnEquip also put the item's options 24000 / 24001 (its HPRecoveryRate / MPRecoveryRate)
//  into PlayerRecoveryAmountAcc (Player +2724, the ResChain 2: + (int)(rate * value) to each recovery —
//  regen_amount, es.cpp); OnRelease takes it out (sub_7174A0).
//  Our leaving takes the pet along: the retail Pet::OnProcess (sub_7385D0) destroys a pet whose owner ref
//  is gone on its next tick (logout, zone change); here it leaves at once (leave_field / on_close).
//
//  Shell::CreatePet (sub_7FD700 -> sub_7FD110): a new object id (sub_411250), kind 4, its CharacterInfo id
//  = sub_6DFB90(the item's look id) — the look option (type 12, sub_6ECEC0) is not attached to our items,
//  so the item's own id; its name = CharacterInfo(sub_6DFB90(item id)).CommonName (sub_622CD0) — here the
//  item's name (like mob_db: the Paula backup's English name of the same model; retail: the Korean
//  CommonName). Pet::Init (sub_738010): current speed 0, the FSM in state 0; SetOwner (sub_738420): at its
//  owner's position, the PetFollowingStrategy following it (sub_738230: distance 80, gap 300 ms, type 2),
//  its speeds = the owner's walk / run / chase (+156/+160/+164, kept in step by the owner's speed setters).
//
//  The appear (Character sub_715550 + Pet vtbl+48 sub_738390), S2C op 8/9 — VERIFIED: client sub_6F02B0 ->
//  the actor factory sub_6EA900 case 4 (the Pet pool sub_6EA580) and the Pet reader sub_6C69A0 (the shared
//  prefix sub_6BCE00, then the owner's u16: it links the pet to that player actor and takes its speeds):
//    [u16 id][u16 kind 4][u16 species][u8 0][u8 0][u16 level: never set for a pet -> 0][u32 +92 flags]
//    [pos cur][pos dest][cstr name] + [u8 action: 3 in FSM state 1 (Pet vtbl+96 sub_7383F0), else 0]
//    [u8 facing][u8 cur speed / 4][u32][u8 0][u32] + [u16 owner id].
//
//  The Pet FSM (Pet::OnEvent sub_738960; ChangeState sub_414B50 = leave (3) then enter (1)):
//    state 0 — enter: the idle timer 0, its end rand(8000..20000). Tick: the strategy's move type = the
//      owner's current one (+92 low bits, 0 at rest); standing still past the end -> S2C 63 [id][0] (client
//      Pet vtbl+220 sub_6C6770: animation 9) and a new end; the owner in state 1 (attacking) -> 1, dead
//      (state 2) -> 2, its hate list not empty (sub_4209E0: Character +232) -> 1.
//    state 1 — enter: S2C 63 [id][2] (client: mode 2); leave: S2C 63 [id][1] (client: back to its walk /
//      run / stand animation). Tick: the move type as above; the owner neither attacking nor with a hate
//      entry -> 0, dead -> 2.
//    state 2 — tick: the owner alive again -> 0.
//  The owner's hate list (+232: who it fights, 5000 ms each) is taken as "a monster fights us" (mobs_).
//
//  PetFollowingStrategy (vtable 0x98384C; base BaseFollowingStrategy, process sub_421340):
//    tick — the squared distance of the CURRENT positions > 80² -> the far time runs; at 300 ms the follow
//      step (sub_4211A0) and it restarts: a move to the owner's current position, S2C 0x0018 [id][cur]
//      [dest][type] (sub_5F5240) when it moves, else S2C 0x0019 [id][cur][type] (sub_5F52F0).
//    move end (30008, sub_4215C0) — still farther than 80: a free cell 5..10 rings around the owner's
//      destination cell (sub_422200(grid, 5, 11)), else that destination; refused -> the owner's current
//      position; refused too -> a warp there: the position set and S2C 145 [id][pos] (sub_5F51B0; client
//      sub_499010 case 145: stop + place). Then S2C 0x0018 when it moves.
//  MoveTo (sub_717CB0 -> PathFindingMoveStrategy::Move sub_42BC10): type 0 counts as 2; the current speed
//  by type (1 walk, 2 run; 3/4 keep it here, as for the player) — a change is S2C 0x01D8 first (Pet vtbl+92
//  sub_738580). The retail path search is not done: a blocked destination cell refuses the move.
//
//  The pet's speech (PetSpeechDb: PetParam by species -> its PetSpeech_* table, the lines per Type):
//    report (sub_738F50, a situation 1..29): the pause it would start = 6000 ms for 6 / 7, else 3000 (set at
//      every report, even a dropped one); 3 first ends a running pause (sub_738FC0); the report waits only when
//      no pause runs and none waits (+1688).
//    tick (sub_738FF0, the head of Pet::OnProcess sub_7385D0): a running pause counts; past its length it ends
//      (and the waiting report with it); a waiting report whose Type (situation - 1, sub_7390B0) picks a line
//      (sub_60CDB0) is said (sub_738900 -> sub_5FBF70: a pet made in a map of Type 14 says nothing) and the
//      pause starts; no line -> it is dropped. Saying picks again (a second rand() — it can come out empty).
//    S2C 592, VERIFIED in the KR client (sub_6F0960 case 592 + the private server's .myown patch at 0x1273940):
//      [u16 pet id][str text][u32: read and dropped][str voice] — the patch keeps a voice of up to 63 bytes and
//      plays it (sub_5AD970) before the 5000 ms balloon (sub_8FF0D0), when the option +0xC707 is on and the
//      actor exists. The 2012 server writes [u16 id][u8 PetParam.Voice][str][str voice][u32 Type][u32 SpeechColor]
//      (the 2014 wire has no Voice byte / colour); the u32 here is the Type.
//    Situations (the callers of sub_738F50; the Types per PetSpeech_VoiceTest, the test pet 4262 = item 1762 —
//    every other table has only 0..4): 1 idle (S2C 63 [0]), 2 battle state enter, 3 owner died (Player::OnDie),
//    4 the owner starts to run (state 0; then held 10 s), 5 equipped (OnEquip, alive), 6 HP <= 30% after a blow
//    (Player vtbl+236 / +240), 7 MP <= 30% (every Player tick), 8 level up, 9 TM level up, 10 sit, 11 rest,
//    12 money by C2S 1036 (not handled here), 13 a pick-up (gelder: Player state 4; item: OnPickupItem with
//    the DB's pick-up kind 0 / 3 — 1 is 21, 2 is 20; not modelled) and quest kinds 2 / 5 done, 14 throwing items
//    away (Player::OnDropItem / the "Drop items" path), 15 quest kinds 1 / 4 / 7 / 8 done, 16 a monster quest's
//    time <= 10 s, 17 drilling starts (state 3), 18 / 19 refine failed / done, 22 a boss killed, 23 Guardian
//    Summon (no system, no lines), the message 1116 (any situation; its senders not found).
//  Not done: the look option, the MyCamp pet release / re-summon (MyCampHelper sub_6FCAE0), Pet::MakeOperatable
//  (msg 30156).
// ─────────────────────────────────────────────────────────────────────────────

namespace {
constexpr int64_t kPetNear2      = 80 * 80;   // sub_738230: vtbl+52 (80) -> +76 = 80*80
constexpr int32_t kPetFollowGap  = 300;       // vtbl+56: +84 = 300 ms
constexpr int     kPetRingInner  = 5;         // sub_4215C0: sub_422200(grid, 5, 11, cell)
constexpr int     kPetRingOuter  = 11;

int rand_between(int a, int b) { return a + std::rand() % (b - a); }   // sub_420AB0
}  // namespace

// sub_6DFB90: the CharacterInfo id of a pet item (1501 Baby Bunny -> 4001).
uint16_t GameClientSession::pet_chr_type(uint32_t id)
{
    if (id < 70000) return static_cast<uint16_t>(id < 15800 ? id + 2500 : id - 8800);
    if (id < 90000) return static_cast<uint16_t>(id - 62800);
    return static_cast<uint16_t>(id - 85500);
}

PacketBuffer GameClientSession::build_pet_appear(uint16_t opcode) const
{
    PacketBuffer p(static_cast<Opcode>(opcode), is_paula());
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    pet_.motion.pos(now, cx, cy);
    p.write<uint16_t>(static_cast<uint16_t>(pet_.oid));
    p.write<uint16_t>(4);                                          // kind (+58): Pet
    p.write<uint16_t>(pet_.species);                               // +436
    p.write<uint8_t>(0);                                           // +440
    p.write<uint8_t>(0);                                           // +1024
    p.write<uint16_t>(0);                                          // +1020 (no level)
    p.write<uint32_t>(pet_.motion.state_flags(now));               // +92: the running move's type
    p.write<uint16_t>(static_cast<uint16_t>(cx)); p.write<uint16_t>(static_cast<uint16_t>(cy));
    p.write<uint16_t>(static_cast<uint16_t>(pet_.motion.tx)); p.write<uint16_t>(static_cast<uint16_t>(pet_.motion.ty));
    const std::string nm = to_client_text(pet_.name);
    p.write_bytes(nm.c_str(), nm.size() + 1);
    // Character sub_715670: sub_7146E0 [action][facing][speed], then the shared prefix's rest
    p.write<uint8_t>(pet_.state == 1 ? 3 : 0);
    p.write<uint8_t>(0);
    p.write<uint8_t>(static_cast<uint8_t>(static_cast<int>(pet_.cur_speed / 4.0f)));   // sub_60C750
    p.write<uint32_t>(0);
    p.write<uint8_t>(0);
    p.write<uint32_t>(0);
    p.write<uint16_t>(static_cast<uint16_t>(object_id_));          // sub_738390: the owner (+1636 -> +56)
    return p;
}

// Shell::CreatePet (sub_7FD700 -> sub_7FD110) + Pet::Init (sub_738010) + SetOwner (sub_738420). Init: PetParam
// of the species (+1660); silent (+1684) when the owner's map (sub_75EB90: its zone object +108) is of Type 14 — at a
// zone enter the pet is made at MCMD_ENTER_ZONE_ACK_OK (sub_40C0F0) before the field enter (sub_7FDA00) of a Player
// built from the transferred state, so there it sees no zone (MapInfo 0, not Type 14): `in_zone` false; the speech
// pause 3000 ms with PetParam Voice, else 1000; the idle end 20000 (set after the state 0's enter).
bool GameClientSession::pet_create(const BagItem& b, const ItemDef& d, bool in_zone)
{
    const uint32_t oid = server_.entity_mgr().alloc_oid();
    if (!oid) {
        LOG_ERROR("Shell::CreatePet - Alloc failed %u, %u", static_cast<unsigned>(object_id_), pet_chr_type(b.item_id));
        return false;
    }
    pet_ = Pet{};
    pet_.active  = true;
    pet_.oid     = oid;
    pet_.item    = b.uid;
    pet_.species = pet_chr_type(b.item_id);
    std::snprintf(pet_.name, sizeof(pet_.name), "%s", d.name.c_str());
    int32_t x, y;
    cur_pos(std::chrono::steady_clock::now(), x, y);
    pet_.motion.rest(x, y);                                        // SetOwner: at the owner (sub_7149C0)
    pet_.last_x = x; pet_.last_y = y;
    pet_.idle_end_ms = 20000;                                     // +1672 (after sub_414B50(0)'s rand)
    pet_.speech = server_.pet_speech_db().find(pet_.species);
    if (in_zone)
        if (const MapDef* md = server_.maps_db().get(zone_id_)) pet_.silent = (md->type == 14);
    pet_.say_pause_ms = (pet_.speech && pet_.speech->voice) ? 3000 : 1000;
    LOG_DEBUG("GameClientSession %u: pet %u (%s, chr %u) made for item %llu at (%d,%d)", session_id(), oid,
              d.name.c_str(), pet_.species, static_cast<unsigned long long>(b.uid), x, y);
    return true;
}

// +2740 / +2744 = the item's options 24000 / 24001 (sub_6E8140, summed); either above 0 puts the element
// into the chain (sub_7172E0).
void GameClientSession::pet_regen_set(const BagItem& b)
{
    int16_t hp = 0, mp = 0;
    for (const ItemOption& o : b.opts) {
        if (o.id == 24000) hp = static_cast<int16_t>(hp + o.value);
        if (o.id == 24001) mp = static_cast<int16_t>(mp + o.value);
    }
    const float fh = static_cast<float>(hp), fm = static_cast<float>(mp);
    if (fh > 0.0f || fm > 0.0f) {
        pet_regen_hp_ = fh;
        pet_regen_mp_ = fm;
        pet_regen_on_ = true;
    }
}

// PetItemHelper::OnLoad (sub_6E6B10): the equipped pet item's pet, op 8 to the area — the login sends it to
// us too (we are in the sector already), a zone enter only to the others (the sector enter shows it to us).
void GameClientSession::pet_load(bool to_self)
{
    if (pet_.active) return;
    const uint64_t uid = equip_[8];
    const BagItem* b = uid ? bag_find_uid(uid) : nullptr;
    const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
    if (!d || d->type != 13) return;
    if (!pet_create(*b, *d, to_self)) return;                    // (to_self: the login, after the sector enter)
    pet_regen_set(*b);
    PacketBuffer p = build_pet_appear(0x0008);                    // sub_5F6F40
    server_.broadcast_zone(zone_id_, p, to_self ? 0 : session_id());
}

// PetItemHelper::OnEquip (sub_6E6C30): the pet, and op 9 (the warp-in) to the area unless we are dead.
void GameClientSession::pet_equipped(const BagItem& b, const ItemDef& d)
{
    if (pet_.active) pet_destroy(true);                           // (Player +1712 is overwritten in retail)
    if (!pet_create(b, d, true)) return;
    pet_regen_set(b);
    if (!dead_) {
        PacketBuffer p = build_pet_appear(0x0009);                // sub_5F6FC0
        server_.broadcast_zone(zone_id_, p, 0);
        pet_speech(5);                                            // sub_738940: equipped
    }
}

// PetItemHelper::OnRelease (sub_6E6DF0): the recovery element out, the pet leaves.
void GameClientSession::pet_released()
{
    pet_regen_on_ = false;
    if (pet_.active) pet_destroy(true);
}

// vtbl+28 (sub_715530) -> Shell::DestroyObject (sub_7FD200 case 4) -> OnLeave (sub_7381F0): S2C 0x000F
// [u16 id][u8 0] to the area (sub_5F7610); its id goes back to the IdManager.
void GameClientSession::pet_destroy(bool to_self)
{
    if (!pet_.active) return;
    PacketBuffer p(static_cast<Opcode>(0x000F), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(pet_.oid));
    p.write<uint8_t>(0);
    server_.broadcast_zone(zone_id_, p, to_self ? 0 : session_id());
    server_.entity_mgr().free_oid(pet_.oid);
    LOG_DEBUG("GameClientSession %u: pet %u leaves", session_id(), pet_.oid);
    pet_ = Pet{};
}

// ChangeState (sub_414B50) on the Pet FSM: leave (3) the old state, enter (1) the new one.
void GameClientSession::pet_set_state(int state)
{
    if (state == pet_.state) return;
    auto action = [&](uint8_t a) {                                 // sub_5F4420: S2C 63 [u16 id][u8] to the area
        PacketBuffer p(static_cast<Opcode>(0x003F), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(pet_.oid));
        p.write<uint8_t>(a);
        server_.broadcast_zone(zone_id_, p, 0);
    };
    if (pet_.state == 1) action(1);                                // state 1, event 3
    pet_.state = state;
    if (state == 0) {                                              // state 0, event 1
        pet_.idle_ms     = 0;
        pet_.idle_end_ms = rand_between(8000, 20000);
    } else if (state == 1) {                                       // state 1, event 1
        action(2);
        pet_speech(2);
    }
}

// sub_738F50: a situation (1..29) for the pet's line.
void GameClientSession::pet_speech(int situation)
{
    if (!pet_.active) return;
    pet_.say_pause_ms = (situation == 6 || situation == 7) ? 6000 : 3000;   // +1700
    if (situation == 3) { pet_.say_on = false; pet_.say_ms = 0; pet_.say_pending = 0; }   // sub_738FC0
    if (!pet_.say_on && !pet_.say_pending) pet_.say_pending = situation;      // +1692 / +1688
}

// sub_738FF0 (Pet::OnProcess sub_7385D0, before the owner check and the FSM).
void GameClientSession::pet_speech_tick(int elapsed_ms)
{
    auto reset = [&]() { pet_.say_on = false; pet_.say_ms = 0; pet_.say_pending = 0; };   // sub_738FC0
    if (pet_.say_on) pet_.say_ms += elapsed_ms;
    if (pet_.say_ms > pet_.say_pause_ms) reset();
    if (!pet_.say_pending) return;
    const int s = pet_.say_pending;
    const int type = (s >= 1 && s <= 29) ? s - 1 : 0;              // sub_7390B0
    if (pet_.speech && PetSpeechDb::pick(*pet_.speech, type)) {
        pet_say(type);
        pet_.say_pending = 0;
        pet_.say_on = true;
    } else {
        reset();
    }
}

// sub_738900: not in a Type-14 map (+1684) -> sub_5FBF70: the line (picked again, sub_60CDB0) -> S2C 592 to the area.
void GameClientSession::pet_say(int type)
{
    if (pet_.silent || !pet_.speech) return;
    const PetSpeechDb::Line* l = PetSpeechDb::pick(*pet_.speech, type);
    if (!l) return;
    const std::string t = to_client_text(l->text);
    const std::string v = to_client_text(l->voice);
    PacketBuffer p(static_cast<Opcode>(592), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(pet_.oid));
    p.write_bytes(t.c_str(), t.size() + 1);
    p.write<uint32_t>(static_cast<uint32_t>(type));
    p.write_bytes(v.c_str(), v.size() + 1);
    server_.broadcast_zone(zone_id_, p, 0);
    LOG_DEBUG("GameClientSession %u: pet %u says (type %d): %s", session_id(), pet_.oid, type, l->text.c_str());
}

// MoveTo (sub_717CB0 -> sub_42BC10): the speeds must be above 0; type 0 counts as 2; the destination cell
// must be free (the path search, sub_415C60); the current speed by type (S2C 0x01D8 on a change).
bool GameClientSession::pet_move_to(int32_t x, int32_t y, uint8_t type, std::chrono::steady_clock::time_point now)
{
    const float walk = run_speed_ * 0.5f, run = run_speed_;         // the owner's +156 / +160
    if (walk <= 0.0f || run <= 0.0f) return false;
    const MapGrid* g = server_.zone_grid(zone_id_);
    if (g && g->at(x / 16, y / 16) == 1) return false;
    const uint8_t t = ((type & 7) == 0) ? 2 : static_cast<uint8_t>(type & 7);
    float speed = pet_.cur_speed;
    if (t == 1) speed = walk;
    else if (t == 2) speed = run;
    if (speed != pet_.cur_speed) {                                 // Pet vtbl+92 (sub_738580 -> sub_5F70C0)
        pet_.cur_speed = speed;
        PacketBuffer sp(static_cast<Opcode>(0x01D8), is_paula());
        sp.write<uint16_t>(static_cast<uint16_t>(pet_.oid));
        sp.write<uint8_t>(static_cast<uint8_t>(static_cast<int>(speed / 4.0f)));
        server_.broadcast_zone(zone_id_, sp, 0);
    }
    int32_t cx, cy;
    pet_.motion.pos(now, cx, cy);
    pet_.motion.start(cx, cy, x, y, pet_.cur_speed, now, t);
    pet_.moving = true;
    return true;
}

// sub_5F5240 when the destination differs from the current position (sub_4158B0): S2C 0x0018 [u16 id][pos cur]
// [pos dest][u8 the strategy's move type] to the area.
void GameClientSession::pet_send_move(std::chrono::steady_clock::time_point now)
{
    int32_t cx, cy;
    pet_.motion.pos(now, cx, cy);
    if (cx == pet_.motion.tx && cy == pet_.motion.ty) return;
    PacketBuffer mv(Opcode::C2S_MOVE, is_paula());
    mv.write<uint16_t>(static_cast<uint16_t>(pet_.oid));
    mv.write<uint16_t>(static_cast<uint16_t>(cx)); mv.write<uint16_t>(static_cast<uint16_t>(cy));
    mv.write<uint16_t>(static_cast<uint16_t>(pet_.motion.tx)); mv.write<uint16_t>(static_cast<uint16_t>(pet_.motion.ty));
    mv.write<uint8_t>(pet_.move_type);
    server_.broadcast_zone(zone_id_, mv, 0);
}

// BaseFollowingStrategy's follow step (vtbl+64 sub_4211A0, no keep-distance for a pet): to the owner's
// current position; refused -> S2C 0x0019 [u16 id][pos cur][u8 type] (sub_5F52F0).
void GameClientSession::pet_follow_step(std::chrono::steady_clock::time_point now)
{
    int32_t ox, oy;
    cur_pos(now, ox, oy);
    if (pet_move_to(ox, oy, pet_.move_type, now)) {
        pet_send_move(now);
        return;
    }
    int32_t cx, cy;
    pet_.motion.pos(now, cx, cy);
    PacketBuffer h(static_cast<Opcode>(0x0019), is_paula());
    h.write<uint16_t>(static_cast<uint16_t>(pet_.oid));
    h.write<uint16_t>(static_cast<uint16_t>(cx));
    h.write<uint16_t>(static_cast<uint16_t>(cy));
    h.write<uint8_t>(pet_.move_type);
    server_.broadcast_zone(zone_id_, h, 0);
}

// PetFollowingStrategy::OnProcessState, the move's end (30008, sub_4215C0).
void GameClientSession::pet_move_end(std::chrono::steady_clock::time_point now)
{
    int32_t ox, oy, px, py;
    cur_pos(now, ox, oy);
    pet_.motion.pos(now, px, py);
    if (dist2(px, py, ox, oy) <= kPetNear2) return;
    // a free cell 5..10 rings around the owner's destination cell (+120 / 16, sub_422100 -> sub_422150)
    int32_t dx = pos_x_, dy = pos_y_;
    if (const MapGrid* g = server_.zone_grid(zone_id_)) {
        int16_t cx = static_cast<int16_t>(pos_x_ / 16), cy = static_cast<int16_t>(pos_y_ / 16);
        if (g->find_drop_cell(kPetRingInner, kPetRingOuter, cx, cy, {})) { dx = cx * 16 + 8; dy = cy * 16 + 8; }
    }
    if (!pet_move_to(dx, dy, pet_.move_type, now) && !pet_move_to(ox, oy, pet_.move_type, now)) {
        // "PetFollowingStrategy::OnProcessState": the warp (sub_7149C0 + sub_5F51B0) to the chosen point
        LOG_DEBUG("PetFollowingStrategy::OnProcessState - pet %u warps to (%d,%d)", pet_.oid, dx, dy);
        pet_.motion.rest(dx, dy);
        pet_.moving = false;
        PacketBuffer w(static_cast<Opcode>(0x0091), is_paula());
        w.write<uint16_t>(static_cast<uint16_t>(pet_.oid));
        w.write<uint16_t>(static_cast<uint16_t>(dx));
        w.write<uint16_t>(static_cast<uint16_t>(dy));
        server_.broadcast_zone(zone_id_, w, 0);
        return;
    }
    pet_send_move(now);
}

// Pet::OnProcess (sub_7385D0 -> sub_6FD5E0): the FSM's tick (Character vtbl+4 -> Pet::OnEvent event 2),
// then the components — the move (its end: 30008) and the PetFollowingStrategy's tick.
void GameClientSession::pet_tick(std::chrono::steady_clock::time_point now, int elapsed_ms)
{
    pet_speech_tick(elapsed_ms);                                   // sub_738FF0 first
    // Our FSM state as the pet reads it (unknown_libname_146): 2 dead, 1 attacking; the hate list (sub_4209E0).
    const int  owner_state = dead_ ? 2 : (attacking_ ? 1 : 0);
    const bool owner_hate  = !mobs_.empty();
    const uint8_t owner_type = static_cast<uint8_t>(motion_.state_flags(now) & 7u);   // sub_41C800(+92)

    if (pet_.state == 0) {
        pet_.move_type = owner_type;                               // strategy vtbl+48
        if (owner_type == 2 && pet_.run_seen != 2) {               // the owner starts to run: the speech 4
            pet_speech(4);
            pet_.run_seen    = 2;
            pet_.run_hold_ms = 10000;
        }
        if (pet_.run_hold_ms <= 0) pet_.run_seen = owner_type;
        else                       pet_.run_hold_ms -= elapsed_ms;
        int32_t px, py;
        pet_.motion.pos(now, px, py);
        if (px != pet_.last_x || py != pet_.last_y) {
            pet_.idle_ms = 0;
            pet_.last_x = px; pet_.last_y = py;
        } else {
            pet_.idle_ms += elapsed_ms;
            if (pet_.idle_ms > pet_.idle_end_ms) {                 // S2C 63 [id][0], the speech 1
                PacketBuffer p(static_cast<Opcode>(0x003F), is_paula());
                p.write<uint16_t>(static_cast<uint16_t>(pet_.oid));
                p.write<uint8_t>(0);
                server_.broadcast_zone(zone_id_, p, 0);
                pet_speech(1);
                pet_.idle_ms     = 0;
                pet_.idle_end_ms = rand_between(8000, 20000);
            }
        }
        if (owner_state == 1)      pet_set_state(1);
        else if (owner_state == 2) pet_set_state(2);
        else if (owner_hate)       pet_set_state(1);
    } else if (pet_.state == 1) {
        pet_.move_type = owner_type;
        if (owner_state != 1) {
            if (owner_state == 2)  pet_set_state(2);
            else if (!owner_hate)  pet_set_state(0);
        }
    } else if (pet_.state == 2) {
        if (owner_state != 2) pet_set_state(0);
    }

    // The move strategy: the arrival posts 30008 (sub_42C330) — PetFollowingStrategy's handler.
    if (pet_.moving && !pet_.motion.moving(now)) {
        pet_.moving = false;
        pet_move_end(now);
    }
    // PetFollowingStrategy's tick (sub_421340, event 2): far -> the far time; 300 ms -> the follow step.
    int32_t ox, oy, px, py;
    cur_pos(now, ox, oy);
    pet_.motion.pos(now, px, py);
    if (dist2(px, py, ox, oy) > kPetNear2) {
        pet_.far_ms += elapsed_ms;
        if (pet_.far_ms >= kPetFollowGap) {
            pet_follow_step(now);
            pet_.far_ms = 0;
        }
    }
    // (else 30011 [1] to the pet: no state of it acts on 1)
}
