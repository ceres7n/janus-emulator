#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
//  GM atcommands (rAthena-style, not retail): @item, @dig, @quest, @cdm, @warp, @jump, @heal, @kill,
//  @save, @levelgift, @sysmega, @pqkill, @where, @speed, @blvl, @tmlvl, @skill, @spawn (18).
//  Entry: handle_chat() reads the C2S 54 text; a leading '@' routes to run_atcommand(),
//  which refuses every command to a character whose characters.authority is below 1 and
//  logs each use at INFO. Feedback goes back as S2C 758 lines (send_server_message).
// ─────────────────────────────────────────────────────────────────────────────

// Player cases 54 / 417 / 713 (sub_73D2A0, read from its disassembly): C2S [text\0]. The client
// (sub_8BC100 "ChatInput") sends 54 for the normal chat, 417 in a zone of type 23 and 713 while its
// avatar observes (+92 0x400); party / guild / whisper lines go through the community, and '/' lines
// are the client's own commands. (Its C2S 55 sender, sub_5A1AB0, is never called and S2C 58 has no
// handler: the server-wide say, sub_5F8430, is out.)
//   54: a player in a MyCamp room hands the message to the room (Player +0x126C / +0x6B0 — no MyCamp
//       here); the conditions 2 / 3 / 8 end first (sub_73C9C0 / sub_73CAF0 / sub_73CCD0, S2C 183 / 318 /
//       816 — only 8, the Mic's head notice, exists here: megaphone.cpp); then the area (the Paula sector
//       = the whole map), the speaker too, gets
//       S2C 57 [u16 id][name\0][text\0] (sub_5F8190; client sub_6F0960 case 57: a 5 s balloon + the
//       chat line), or S2C 181 when the speaker stands on a cell whose byte 2 is 1 (sub_75EAA0 ->
//       sub_5F8230; client: a 30 s balloon, a type-2 line and data/sound/em_bulb.wav).
//   417: the same with S2C 417 (sub_5F8070; client case 417: the chat line) or 181.
//   713: only an observer (+92 0x80, a guild war's) speaks: S2C 712 to the zone (sub_5F82D0; client
//        "GGObserverSayMark") — no guild war here, nobody is one.
// A line starting with '@' is our GM command instead (atcommands below, not retail).
void GameClientSession::handle_chat(PacketBuffer& pkt, Opcode op)
{
    std::string text;
    try {
        while (pkt.rpos() < pkt.size()) {
            const char c = static_cast<char>(pkt.read<uint8_t>());
            if (!c) break;
            text += c;
        }
    } catch (const std::exception&) { return; }

    if (op == Opcode::C2S_CHAT && !text.empty() && text[0] == '@') {
        run_atcommand(text);
        return;
    }
    head_notice_end();                                             // the conditions 2 / 3 / 8 end first
    if (op == static_cast<Opcode>(0x02C9)) return;                 // 713: not an observer
    // Hardening (not retail): the relayed line is cut to the client's own chat-box limit (kClientChatMax).
    cut_client_text(text, kClientChatMax);

    uint16_t say = (op == Opcode::C2S_CHAT) ? 0x0039 : 0x01A1;
    if (const MapGrid* g = server_.zone_grid(zone_id_)) {
        const int16_t x = static_cast<int16_t>(pos_x_), y = static_cast<int16_t>(pos_y_);
        const int cx = x / 16, cy = y / 16;
        if (cx >= 0 && cy >= 0 && cx < g->w && cy < g->h &&
            g->layer2[static_cast<size_t>(g->w) * static_cast<size_t>(cy) + static_cast<size_t>(cx)] == 1)
            say = 0x00B5;                                          // 181
    }
    PacketBuffer p(static_cast<Opcode>(say), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(object_id_));
    p.write_bytes(reinterpret_cast<const uint8_t*>(char_name_), std::strlen(char_name_) + 1);
    p.write_bytes(reinterpret_cast<const uint8_t*>(text.c_str()), text.size() + 1);
    send(p);
    server_.broadcast_zone(zone_id_, p, session_id());
}

void GameClientSession::send_server_message(const std::string& msg)
{
    // A system line: S2C 758 (0x02F6) [text\0] (client sub_6F0960 case 758 -> sub_8B8140(text, 5)): no
    // sender name, chat type 5 = the ChatMsgColor "Message" colour (0xFF9CFF00, green).
    PacketBuffer p(static_cast<Opcode>(0x02F6), is_paula());
    p.write_bytes(reinterpret_cast<const uint8_t*>(msg.c_str()), msg.size() + 1);
    send(p);
}

void GameClientSession::run_atcommand(const std::string& text)
{
    if (text.size() < 2 || text[0] != '@') return;

    std::istringstream iss(text.substr(1));
    std::string cmd; iss >> cmd;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    std::vector<std::string> a; std::string tok;
    while (iss >> tok) a.push_back(tok);

    // GM gate: character's authority level (set it in the DB to use these). Every command below needs it.
    if (authority_ < 1) {
        LOG_INFO("GameClientSession %u: '@%.32s' denied (authority=%u)", session_id(), cmd.c_str(), authority_);
        return;
    }
    // The GM audit line (release logging policy: GM command use is logged at INFO).
    LOG_INFO("GameClientSession %u: GM command by char %u '%s' (authority %u): %.200s", session_id(), char_uid_,
             char_name_, authority_, text.c_str());

    auto num = [](const std::string& s, long d = 0) -> long {
        try { return std::stol(s); } catch (...) { return d; }
    };

    if (cmd == "item") {
        if (a.empty()) { send_server_message("usage: @item <id> [amount]"); return; }
        uint32_t id  = static_cast<uint32_t>(num(a[0]));
        uint16_t amt = a.size() > 1 ? static_cast<uint16_t>(num(a[1], 1)) : 1;
        if (amt < 1) amt = 1;
        const ItemDef* d = server_.item_db().get(id);
        if (!d) {
            char buf[96]; snprintf(buf, sizeof(buf), "@item: item %u does not exist (item_db.yml)", id);
            send_server_message(buf);
            return;
        }
        bool has_stack = false;
        if (d->stackable())
            for (const BagItem& b : bag_) if (b.item_id == id) { has_stack = true; break; }
        if (!has_stack && bag_.size() >= 300) { send_server_message("@item: the bag is full (300)"); return; }
        if (d->cls != 1) amt = 1;              // a rare item is always one
        // a new drill comes with its DrillLife as its count (the item build sub_6EBAD0)
        if (d->cls == 2 && d->type == 6)
            if (const DrillParam* dp = server_.drill_db().get(id)) amt = static_cast<uint16_t>(dp->drill_life);
        // Into the bag exactly like a pick-up's record (no weight check: a GM gift).
        const BagRecord rec = bag_add(*d, amt);
        send_bag_record(rec);
        LOG_INFO("GameClientSession %u: @item %u x%u (%s) -> uid %llu (%s)", session_id(), id, amt, d->name.c_str(),
                 static_cast<unsigned long long>(rec.uid), rec.type == 1 ? "stack" : "new");
        { char buf[128]; snprintf(buf, sizeof(buf), "@item: %s x%u", d->name.c_str(), amt); send_server_message(buf); }
    }
    else if (cmd == "dig") {
        // @dig — the cell we stand on: its ground nature (MapAttr Attr; 6 = no drilling) and the depth
        // of the item buried there, and the zone's buried items (drill.cpp).
        int32_t x, y;
        cur_pos(std::chrono::steady_clock::now(), x, y);
        const MapGrid* g = server_.zone_grid(zone_id_);
        const int16_t cx = static_cast<int16_t>(static_cast<int16_t>(x) / 16);
        const int16_t cy = static_cast<int16_t>(static_cast<int16_t>(y) / 16);
        const int nature = g ? g->nature_at(cx, cy) : -1;
        const int blocked = g ? (g->at(cx, cy) == 1) : -1;
        const int16_t depth = server_.map_item_depth(zone_id_, static_cast<int16_t>(x), static_cast<int16_t>(y));
        size_t laid = 0; int32_t unblocked = 0;
        server_.map_item_stats(zone_id_, laid, unblocked);
        const MapDef* md = server_.maps_db().get(zone_id_);
        char buf[256];
        snprintf(buf, sizeof(buf), "@dig: cell (%d,%d) nature %d%s, item %s%d | map %d: %zu items in %d cells "
                 "(%.0f%%, depth %d..%d) | Crazy Drill: %s, gauge %.2f/%.0f", cx, cy, nature,
                 blocked == 1 ? " [blocked]" : "", depth < 0 ? "none " : "at ", depth < 0 ? 0 : depth, zone_id_,
                 laid, unblocked, md ? static_cast<double>(md->item_percentage) : 0.0, md ? md->min_item_depth : 0,
                 md ? md->max_item_depth : 0, cdm_.active ? "ON" : (cdm_enabled() ? "off" : "no skill 5019"),
                 static_cast<double>(cdm_.gauge), static_cast<double>(cdm_.gauge_max));
        send_server_message(buf);
    }
    else if (cmd == "quest") {
        // @quest — the normal quests in progress (QuestInv: qid, run, what they ask for and whether the bag
        // has it); @quest <qid> — that quest's history (char_quest_history: times done, last date).
        if (!a.empty()) {
            const int qid = static_cast<int>(num(a[0]));
            const QuestDef* d = server_.quest_db().get(qid);
            const DBResult h = server_.db().query("SELECT com_count, quest_completed_date FROM char_quest_history "
                                                  "WHERE char_uid=%u AND quest_id=%d AND monster_id=0", char_uid_, qid);
            char buf[256];
            snprintf(buf, sizeof(buf), "@quest %d (%s): MaxCount %d, done %s time(s)%s%s", qid,
                     d ? d->name.c_str() : "does not exist", d ? d->max_count : 0,
                     h.empty() ? "0" : h[0].at("com_count").c_str(), h.empty() ? "" : ", last ",
                     h.empty() ? "" : h[0].at("quest_completed_date").c_str());
            send_server_message(buf);
            return;
        }
        int n = 0;
        for (const QuestEntry& e : quests_) {
            if (!e.used) continue;
            ++n;
            char buf[256];
            snprintf(buf, sizeof(buf), "@quest: %d (%s) run %d, items %s", e.qid, e.def ? e.def->name.c_str() : "?",
                     e.count, (e.def && quest_items_ok(*e.def, e.def->options == 2)) ? "OK" : "missing");
            send_server_message(buf);
        }
        if (!n) send_server_message("@quest: no quest in progress");
    }
    else if (cmd == "cdm") {
        // @cdm — the Crazy Drill Mode now, like the retail test hook (Player case 1029 with 0x1392 ->
        // sub_76A440 -> sub_7AFA20(this, 1)): only with skill 5019 (@skill 5019).
        if (!cdm_enabled()) { send_server_message("@cdm: precisa da skill 5019 (Crazy Excavation) — use @skill 5019"); return; }
        if (cdm_.active) { send_server_message("@cdm: o Crazy Drill ja esta ativo"); return; }
        cdm_activate(1);
        char buf[96];
        snprintf(buf, sizeof(buf), "@cdm: Crazy Drill ativo por %d s", cdm_.remain_ms / 1000);
        send_server_message(buf);
    }
    else if (cmd == "warp") {
        if (a.empty()) { send_server_message("usage: @warp <map> [x y]"); return; }
        // Only the maps of db/maps_db.yml exist (retail BuildWorld builds one Zone per MapInfoEx row).
        const long zl = num(a[0], -1);
        const MapDef* md = server_.maps_db().get(static_cast<int>(zl));
        if (!md) { send_server_message("@warp: map " + a[0] + " does not exist (maps_db.yml)"); return; }
        int16_t z = static_cast<int16_t>(md->id);
        // Seamless portal map-move (reverse/specs/07 §2.7): RELOCATE(154) -> client CMD_ENTER_ZONE(156)
        // -> 437+157. No relog, no spawn-batch re-run. Without coords: the zone's default arrival,
        // as the retail destination resolves an unset position (sub_8108F0: type-2 / type-1 map
        // point, else (500,400)).
        int32_t x = 500, y = 400;
        server_.maps_db().default_arrival(z, x, y);
        if (a.size() > 1) x = static_cast<int32_t>(num(a[1]));
        if (a.size() > 2) y = static_cast<int32_t>(num(a[2]));
        begin_field_change(z, x, y);
        LOG_INFO("GameClientSession %u: @warp -> seamless field change zone=%d (%d,%d)", session_id(), z, x, y);
    }
    else if (cmd == "jump") {
        int32_t x = a.size() > 0 ? static_cast<int32_t>(num(a[0])) : pos_x_;
        int32_t y = a.size() > 1 ? static_cast<int32_t>(num(a[1])) : pos_y_;
        pos_x_ = x; pos_y_ = y;
        motion_.rest(x, y);
        LOG_INFO("GameClientSession %u: @jump (%d,%d)", session_id(), x, y);
    }
    else if (cmd == "heal") {
        hp_ = hp_max_; mp_ = mp_max_;
        LOG_INFO("GameClientSession %u: @heal -> hp=%d mp=%d", session_id(), hp_, mp_);
    }
    else if (cmd == "kill") {
        hp_ = 0;
        LOG_INFO("GameClientSession %u: @kill (self)", session_id());
    }
    else if (cmd == "save") {
        save_position();
        LOG_INFO("GameClientSession %u: @save", session_id());
    }
    else if (cmd == "levelgift") {
        // @levelgift <level>  -> the gift box rewards of that level (PostBoxLevelUpGiftInfo), as if reached
        // @levelgift          -> a sync of the post box now
        if (!a.empty()) post_level_up(static_cast<int>(num(a[0])));
        else post_sync();
        send_server_message(a.empty() ? "@levelgift: post box synced" : "@levelgift: sent (open the gift box)");
    }
    else if (cmd == "sysmega") {
        // @sysmega <text> -> a system megaphone (OnCCmdSysMegaphone: "[트릭스터] island - text", queued as a GM message)
        std::string text;
        for (size_t i = 0; i < a.size(); ++i) text += (i ? " " : "") + a[i];
        if (text.empty()) { send_server_message("usage: @sysmega <text>"); return; }
        server_.sys_megaphone("트릭스터", text);
        send_server_message("@sysmega: queued");
    }
    else if (cmd == "pqkill") {
        // @pqkill [n] -> n kills (1..500) of our running party quest's monster for us, by the party's own path
        // (SYSCMD 5557, party_quest.cpp) — a test helper
        const int n = a.empty() ? 1 : static_cast<int>(num(a[0]));
        if (!pq_.active()) { send_server_message("@pqkill: no party quest running"); return; }
        for (int i = 0; i < n && i < 500; ++i) server_.party().pq_kill(char_uid_, pq_.monster);
        send_server_message("@pqkill: done");
    }
    else if (cmd == "where") {
        LOG_INFO("GameClientSession %u: @where zone=%d (%d,%d)", session_id(), zone_id_, pos_x_, pos_y_);
        char buf[96]; snprintf(buf, sizeof(buf), "@where: zone=%d (%d,%d)", zone_id_, pos_x_, pos_y_);
        send_server_message(buf);
    }
    else if (cmd == "speed") {
        // @speed            -> run speed back to the default 160
        // @speed <n>        -> run speed = n (client actor+120 units); walking is always half of it
        // Applied live with S2C 0x01D8 (the verified speed packet, no re-appear). Per-session only.
        constexpr float kDefaultRun = 160.0f;
        float want = kDefaultRun;
        if (!a.empty()) {
            long v = num(a[0]);
            if (v < 4)    v = 4;               // speed byte >= 1 (0 would walk in place)
            if (v > 1020) v = 1020;            // speed byte <= 255
            want = static_cast<float>(v);
        }
        run_speed_ = want;
        const bool walking = motion_.type == 1;
        set_current_speed(walking ? run_speed_ * 0.5f : run_speed_);
        { char buf[96]; snprintf(buf, sizeof(buf),
              "@speed: correr = %d, andar = %d", static_cast<int>(run_speed_), static_cast<int>(run_speed_ * 0.5f));
          send_server_message(buf); }
        LOG_INFO("GameClientSession %u: @speed -> run=%.0f walk=%.0f cur=%.0f", session_id(),
                 run_speed_, run_speed_ * 0.5f, cur_speed_);
    }
    else if (cmd == "blvl") {
        // @blvl <±X> — raise/lower the BASE level by X (1..400).
        //  Raise = the game's own level-up (retail grant sub_717780 + distributor sub_717590): per level
        //    gained every stat slot += its growth weight (P->AP/AC/DX, M->MaxMP/MA/MD, S->MaxWT/DA/LK,
        //    C->MaxHP/DP/HV), unspent += 4, full heal; then S2C 0x0027 (client plays the level-up
        //    effect + "etc_levelup" + LevelUpPtsStr). Manual allocations are kept.
        //  Lower (GM-only; the retail server has no level-down path): re-derive the points from the
        //    growth graph for the new level, then S2C 0x0275 stat refresh.
        int delta  = a.empty() ? 0 : static_cast<int>(num(a[0]));
        int oldlv  = static_cast<int>(level_);
        int newlv  = oldlv + delta;
        if (newlv < 1)   newlv = 1;
        if (newlv > 400) newlv = 400;
        const int gained = newlv - oldlv;
        if (gained == 0) { send_server_message("@blvl: level unchanged (limits 1..400)"); return; }
        level_ = static_cast<int16_t>(newlv);
        exp_   = 0;
        if (gained > 0) {
            grant_levels(gained);                   // the retail level-up: growth, full heal, S2C 0x0027
        } else {
            init_accpts_from_growth();
            hp_ = hp_max_; mp_ = mp_max_;
            send_stat_refresh();                    // S2C 0x0275
        }
        save_position();                            // persists level/exp/hp/mp + the stat points
        { char buf[96]; snprintf(buf, sizeof(buf), "@blvl -> level %d (HP %d / MP %d, %d pts livres)",
              newlv, hp_max_, mp_max_, attr_pts_); send_server_message(buf); }
        LOG_INFO("GameClientSession %u: @blvl %+d -> level %d", session_id(), gained, newlv);
    }
    else if (cmd == "tmlvl") {
        // @tmlvl <±X> — raise/lower the TM level by X (0..500). Each TM level gained is a retail TM
        // level-up (sub_75D6F0: +1 TM point, Player +4592); each one taken off takes its point back
        // (not below 0). S2C 0x00F5 = the TM level-up packet, verified on both ends: retail builder
        // sub_5ED5A0 [u8 type][u16 TM level][u32 TM exp][u16 TM points]; client case 0xF5 in sub_6B4E00
        // reads exactly that (TM points -> avatar+26684, the skill window). Persisted.
        int delta = a.empty() ? 0 : static_cast<int>(num(a[0]));
        int newtm = static_cast<int>(tm_level_) + delta;
        if (newtm < 0)   newtm = 0;
        if (newtm > 500) newtm = 500;
        const int gained = newtm - static_cast<int>(tm_level_);
        tm_level_  = static_cast<int16_t>(newtm);
        tm_exp_    = 0;
        tm_points_ = std::max(0, tm_points_ + gained);
        send_tm_level_up();                               // S2C 0x00F5 [ftype][TM lv][TM exp][TM pts]
        save_stat_points();
        { char buf[80]; snprintf(buf, sizeof(buf), "@tmlvl -> TM level %d, TM points %d", newtm, tm_points_);
          send_server_message(buf); }
        LOG_INFO("GameClientSession %u: @tmlvl %+d -> TM level %d, TM points %d", session_id(), gained, newtm,
                 tm_points_);
    }
    else if (cmd == "skill") {
        // @skill <id> [level]  — learn/set a class skill (SkillParam2 id) at level (default 1,
        //                         clamped to MaxSKLV); level 0 forgets it. Stored in char_skills; the
        //                         list reaches the client at login (S2C 271), so relog to see it.
        if (a.empty()) { send_server_message("usage: @skill <id> [level] (0 = remove)"); return; }
        const int sid = static_cast<int>(num(a[0]));
        const SkillDef* d = server_.skill_db().get(sid);
        if (!d) { send_server_message("@skill: id " + a[0] + " is not in SkillParam2"); return; }
        int lv = a.size() > 1 ? static_cast<int>(num(a[1])) : 1;
        if (lv > d->max_level) lv = d->max_level;
        if (lv <= 0) {
            server_.db().execute("DELETE FROM char_skills WHERE char_uid=%u AND skill_id=%d", char_uid_, sid);
            skills_.erase(std::remove_if(skills_.begin(), skills_.end(),
                                         [&](const LearnedSkill& s) { return s.id == sid; }), skills_.end());
        } else {
            server_.db().execute("INSERT INTO char_skills (char_uid, skill_id, level) VALUES (%u, %d, %d) "
                                 "ON DUPLICATE KEY UPDATE level=%d", char_uid_, sid, lv, lv);
            auto it = std::find_if(skills_.begin(), skills_.end(),
                                   [&](const LearnedSkill& s) { return s.id == sid; });
            if (it != skills_.end()) it->level = static_cast<uint8_t>(lv);
            else if (skills_.size() < 80) skills_.push_back({ static_cast<uint16_t>(sid), static_cast<uint8_t>(lv) });
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "@skill %d (%s) level %d - log in again to see it in the skill window",
                 sid, lv <= 0 ? "removed" : "ok", lv <= 0 ? 0 : lv);
        send_server_message(buf);
        LOG_INFO("GameClientSession %u: @skill %d '%s' type=%d level=%d", session_id(), sid,
                 d->name.c_str(), d->type, lv);
    }
    else if (cmd == "spawn") {
        // @spawn mob <monId> [name]   — monId is the retail MonsterParamEx2 ID (client renders monNNN)
        // @spawn npc <npcId> [name]   — npcId = client NPC species (renders npcNNN)
        // Spawns at the GM's current zone+position and broadcasts the appear to the zone.
        if (a.empty()) { send_server_message("usage: @spawn mob|npc [id] [name] (mob e.g. 2000 2015 2050 2185)"); return; }
        std::string what = a[0];
        std::transform(what.begin(), what.end(), what.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        // species must be a valid client CharacterInfo key or the actor spawns with no model
        // (invisible). Default mob = 2183 (Torobbie, Lv1 HP100 Ap4 Dp0) — a low-level, testable
        // mob (2015 Electric Snail is a Lv164 that one-shots a low char). If a low id renders
        // invisible it lacks a model; @spawn mob 2015 is the known-good-model fallback.
        uint16_t species = (a.size() >= 2) ? static_cast<uint16_t>(num(a[1]))
                                           : static_cast<uint16_t>(what == "npc" ? 0 : 2183);
        if (what == "npc" && species == 0) {
            send_server_message("usage: @spawn npc <id> [name]"); return;
        }
        std::string name = (a.size() > 2) ? a[2] : (what == "npc" ? "TestNpc" : "TestMob");

        if (what == "mob" || what == "monster") {
            MonsterEntity m;
            m.zone = zone_id_; m.pos_x = pos_x_; m.pos_y = pos_y_;
            m.species = species; m.mob_id = species; m.move_speed = 20;
            // Real stats from the mob_db (MonsterParamEx2). MaxHp here == the client's HP-bar max,
            // so the bar tracks our HP 1:1. Combat uses ap/dp/hv/lk; exp_reward on kill.
            if (const MobDef* d = server_.mob_db().get(species)) {
                m.level = static_cast<int16_t>(d->level);
                m.hp = m.hp_max = (d->max_hp > 0 ? d->max_hp : 100);
                m.ap = d->ap; m.ac = d->ac; m.dx = d->dx; m.dp = d->dp; m.hv = d->hv; m.lk = d->lk;
                m.da = d->da; m.ma = d->ma; m.md = d->md; m.debuff_set = d->debuff_set;
                m.acc_formula = d->acc_formula;
                std::copy(std::begin(d->resist), std::end(d->resist), std::begin(m.resist));
                m.exp_reward = static_cast<uint32_t>(d->exp);
                m.tm_reward  = static_cast<uint32_t>(d->tm);
                // Speeds as CommonMonster::Initialize (sub_7287E0): chase = (walk+run)/2 and the
                // CURRENT speed starts at WalkSpeed. The appear speed byte is (int)(current/4)
                // (sub_7146E0 -> sub_60C750); typed chase moves later switch it via 0x01D8.
                m.walk_speed    = d->walk;
                m.run_speed     = d->run;
                m.chase_speed   = (d->walk + d->run) / 2.0f;
                m.cur_speed     = d->walk;
                m.move_speed    = static_cast<uint8_t>(std::min(255, static_cast<int>(m.cur_speed / 4.0f)));
                m.chase_time_ms = d->chase_time_ms;
                m.aggressive    = (d->type == 1);          // AggressiveMonster (sub_721D50)
                m.view_range    = 200;
                m.name = (a.size() > 2) ? name : d->name;   // explicit name overrides the db name
            } else {
                m.name = name; m.level = 1; m.hp = m.hp_max = 100;   // placeholder: species not in db
                m.move_speed = 0;                                       // no speeds -> static
            }
            m.motion.rest(m.pos_x, m.pos_y);
            uint32_t oid = server_.entity_mgr().spawn_monster(m);
            server_.broadcast_entity_spawn(oid);
            char buf[96]; snprintf(buf, sizeof(buf), "@spawn mob %u (oid=%u) @ (%d,%d)", species, oid, pos_x_, pos_y_);
            send_server_message(buf);
            LOG_INFO("GameClientSession %u: @spawn mob species=%u oid=%u zone=%d (%d,%d)",
                     session_id(), species, oid, zone_id_, pos_x_, pos_y_);
        } else if (what == "npc") {
            NpcEntity n;
            n.zone = zone_id_; n.pos_x = pos_x_; n.pos_y = pos_y_;
            n.species = species; n.npc_id = species; n.name = name;
            uint32_t oid = server_.entity_mgr().spawn_npc(n);
            server_.broadcast_entity_spawn(oid);
            char buf[96]; snprintf(buf, sizeof(buf), "@spawn npc %u (oid=%u) @ (%d,%d)", species, oid, pos_x_, pos_y_);
            send_server_message(buf);
            LOG_INFO("GameClientSession %u: @spawn npc species=%u oid=%u zone=%d (%d,%d)",
                     session_id(), species, oid, zone_id_, pos_x_, pos_y_);
        } else {
            send_server_message("usage: @spawn mob|npc <id> [name]");
        }
    }
    else {
        LOG_INFO("GameClientSession %u: unknown atcommand '@%s'", session_id(), cmd.c_str());
        send_server_message("unknown command: @" + cmd);
    }
}
