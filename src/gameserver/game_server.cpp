#include "game_server.hpp"
#include "game_session.hpp"
#include "system_session.hpp"
#include "logger.hpp"
#include "paula_crypt.hpp"
#include "client_text.hpp"
#include "interserver/interserver_opcodes.hpp"
#include <chrono>
#include <random>

cGameServer::cGameServer(asio::io_context& ioc)
    : ioc_(ioc), acceptor_(ioc), system_acceptor_(ioc), heartbeat_timer_(ioc), accept_retry_timer_(ioc),
      system_accept_retry_timer_(ioc), item_timer_(ioc), mega_timer_(ioc) {}

bool cGameServer::init(const Config& cfg)
{
    // MariaDB (same client lib as MySQL).
    if (!db_.connect(
            cfg.get_str("db_host", "127.0.0.1"),
            cfg.get_str("db_user", "root"),
            cfg.get_str("db_pass", ""),
            cfg.get_str("db_name", "janus"),
            static_cast<uint16_t>(cfg.get_int("db_port", 3307))))
        return false;

    if (cfg.get_bool("paula_mode", true)) {
        std::string sbox_path = cfg.get_str("sbox_path", "tools/capture/sbox_cora.bin");
        if (!janus::PaulaCrypt::Instance().LoadSbox(sbox_path)) {
            // (without the cipher table no KR client can talk to us: refuse to start instead of running uselessly)
            LOG_FATAL("GameServer: paula_mode on but the S-box failed to load from %s", sbox_path.c_str());
            return false;
        }
        LOG_INFO("GameServer: Paula protocol enabled (S-box loaded)");
    }

    // Monster stat table (real HP/Ap/Dp/EXP per species from MonsterParamEx2). Without it, spawns
    // fall back to minimal placeholder stats.
    if (!mob_db_.load(cfg.get_str("mob_db_path", "db/mob_db.yml")))
        LOG_ERROR("GameServer: mob_db failed to load — monster stats will be placeholders");

    // Character stat + EXP tables (retail gulim13/gulim14). Required for real player stats; without
    // them the player keeps the minimal fallback stats.
    if (!stat_table_.load(cfg.get_str("stat_table_path", "db/stat_table.fnt2")))
        LOG_ERROR("GameServer: stat_table failed to load — player stats will be placeholders");
    if (!exp_table_.load(cfg.get_str("exp_table_path", "db/exp_table.fnt2")))
        LOG_ERROR("GameServer: exp_table failed to load — level EXP thresholds unavailable");
    if (!tm_table_.load(cfg.get_str("tm_level_db_path", "db/tm_level_db.yml")))
        LOG_ERROR("GameServer: tm_level_db failed to load — TM EXP thresholds unavailable");
    // Attack ranges per character/monster type (CharacterInfo). Without it everyone gets the
    // retail defaults (71..86, chase 200).
    if (!char_range_.load(cfg.get_str("char_range_path", "db/char_range_db.yml")))
        LOG_ERROR("GameServer: char_range failed to load — using default attack ranges");
    // Map table (MapInfoEx + map arrival points): the maps that exist, return towns and arrival
    // positions. Without it every arrival falls back to the retail (500,400).
    if (!maps_db_.load(cfg.get_str("maps_db_path", "db/maps_db.yml")))
        LOG_ERROR("GameServer: maps_db failed to load — arrivals fall back to (500,400)");
    // Class skills (SkillParam2): the ids a learned-skill list (S2C 271) may carry.
    if (!skill_db_.load(cfg.get_str("skill_db_path", "db/skill_db.yml")))
        LOG_ERROR("GameServer: skill_db failed to load — learned skills cannot be validated");
    // Skill effect tables (ESAII invocation conditions per level, ESAction values): skill use.
    if (!esa_db_.load(cfg.get_str("esa_db_path", "db/esa_db.yml")))
        LOG_ERROR("GameServer: esa_db failed to load — skills cannot be used");
    // Items (ItemParam2/Ex2/CM2/Ex) and the monster drop tables: monster loot on a kill.
    if (!item_db_.load(cfg.get_str("item_db_path", "db/item_db.yml")))
        LOG_ERROR("GameServer: item_db failed to load — monsters drop nothing");
    if (!drop_db_.load(cfg.get_str("mon_item_drop_db_path", "db/mon_item_drop_db.yml"),
                       cfg.get_str("mon_item_table_db_path", "db/mon_item_table_db.yml")))
        LOG_ERROR("GameServer: drop tables failed to load — monsters drop no items");
    // Rare items' random stats and compound slots (RandomStatValueTable / RandomStatImportTable).
    if (!random_stat_db_.load(cfg.get_str("random_stat_db_path", "db/random_stat_db.yml"),
                              cfg.get_str("random_stat_import_db_path", "db/random_stat_import_db.yml")))
        LOG_ERROR("GameServer: random stat tables failed to load — new rare items get no random stats");
    // The refine tables (RefineLevelTable / ItemRefineTable0..6 / RefineSupportItemLevel).
    if (!refine_db_.load(cfg.get_str("item_refine_db_path", "db/item_refine_db.yml"),
                         cfg.get_str("item_refine_table_db_path", "db/item_refine_table_db.yml"),
                         cfg.get_str("refine_support_db_path", "db/refine_support_db.yml")))
        LOG_ERROR("GameServer: refine tables failed to load — Item_Refine refuses every request");
    // The mature compound (Equip_Property / Maturing_Compound / Maturing_Compound_Help).
    if (!maturing_db_.load(cfg.get_str("equip_property_db_path", "db/equip_property_db.yml"),
                           cfg.get_str("maturing_compound_db_path", "db/maturing_compound_db.yml"),
                           cfg.get_str("maturing_help_db_path", "db/maturing_help_db.yml")))
        LOG_ERROR("GameServer: maturing tables failed to load — Item_Growth refuses every request");
    // Skill cards (Item_SkillCardParam): what a skill card item teaches.
    if (!skill_card_db_.load(cfg.get_str("skill_card_db_path", "db/skill_card_db.yml")))
        LOG_ERROR("GameServer: skill_card_db failed to load — skill cards cannot be learned");
    // Resurrection items (Item_RebirthParam): what an Effect-29 item restores.
    if (!item_rebirth_db_.load(cfg.get_str("item_rebirth_db_path", "db/item_rebirth_db.yml")))
        LOG_ERROR("GameServer: item_rebirth_db failed to load — resurrection items cannot be used");
    // The skills' HP / MP preconditions (PreESAICondition, sub_890DD0).
    if (!pre_esaic_db_.load(cfg.get_str("pre_esaic_condition_db_path", "db/pre_esaic_condition_db.yml")))
        LOG_ERROR("GameServer: pre_esaic_condition_db failed to load — skills fire without their HP / MP rates");
    // Guns (Item_GunParam2): the attack ranges of an equipped gun and the shot checks.
    if (!gun_db_.load(cfg.get_str("gun_db_path", "db/gun_db.yml")))
        LOG_ERROR("GameServer: gun_db failed to load — guns cannot shoot");
    // The maps' .md3 files (zone attributes: dropped items avoid blocked cells), read on first use;
    // their ground natures go through MapAttr (loaded before the first grid).
    map_attr_.set_dir(cfg.get_str("map_path", "data/map"));
    if (!map_attr_.load_attr_table(cfg.get_str("map_attr_db_path", "db/map_attr_db.yml")))
        LOG_ERROR("GameServer: map_attr_db failed to load — every ground is of nature 0");
    // Drills (Item_DrillParam, DrillGradeExpRate, StressPointInfo) and what they dig up per map
    // (MapItem_All, MapItem_ETC_Table, MapItem_Event_Add).
    if (!drill_db_.load(cfg.get_str("drill_db_path", "db/drill_db.yml"),
                        cfg.get_str("drill_grade_exp_db_path", "db/drill_grade_exp_db.yml"),
                        cfg.get_str("stress_point_db_path", "db/stress_point_db.yml")))
        LOG_ERROR("GameServer: drill_db failed to load — drilling is unavailable");
    // Other clients' values of some drills (a Season 2 client keys its dig results with its own data)
    drill_db_.load_client(cfg.get_str("drill_client_db_path", "db/drill_client_db.yml"));
    if (!map_item_db_.load(cfg.get_str("map_item_db_path", "db/map_item_db.yml"),
                           cfg.get_str("map_item_table_db_path", "db/map_item_table_db.yml"),
                           cfg.get_str("map_item_event_add_db_path", "db/map_item_event_add_db.yml")))
        LOG_ERROR("GameServer: map_item_db failed to load — drills dig up nothing");
    // The maps' monster tables (Mon_<area>) and boss tables (BossMon_*): the MonsterManager of every map.
    if (!mon_spawn_db_.load(cfg.get_str("mon_spawn_db_path", "db/mon_spawn_db.yml")))
        LOG_ERROR("GameServer: mon_spawn_db failed to load — the maps get no monsters");
    boss_mon_db_.load(cfg.get_str("boss_mon_db_path", "db/boss_mon_db.yml"));
    build_regen();
    // The maps' shop tables (Shop_*) and their item lists: the ShopNpcs, placed with the NPCs.
    if (!shop_db_.load(cfg.get_str("shop_db_path", "db/shop_db.yml"), cfg.get_str("shop_item_db_path", "db/shop_item_db.yml")))
        LOG_ERROR("GameServer: shop_db failed to load — the maps get no shops");
    // The maps' skill masters (SkillShopTalk_*), their shop lists and the skills they master.
    if (!skill_shop_db_.load(cfg.get_str("skill_shop_db_path", "db/skill_shop_db.yml"),
                             cfg.get_str("skill_shop_item_db_path", "db/skill_shop_item_db.yml"),
                             cfg.get_str("limit_skill_master_db_path", "db/limit_skill_master_db.yml"),
                             cfg.get_str("skill_master_db_path", "db/skill_master_db.yml")))
        LOG_ERROR("GameServer: skill_shop_db failed to load — no skill masters");
    if (!present_item_db_.load(cfg.get_str("present_item_db_path", "db/present_item_db.yml")))
        LOG_ERROR("GameServer: present_item_db failed to load — the gift boxes give nothing");
    if (!skill_scroll_db_.load(cfg.get_str("skill_scroll_db_path", "db/skill_scroll_db.yml")))
        LOG_ERROR("GameServer: skill_scroll_db failed to load — the skill scrolls cannot be used");
    if (!random_port_db_.load(cfg.get_str("random_port_db_path", "db/random_port_db.yml")))
        LOG_ERROR("GameServer: random_port_db failed to load — the brooms cannot be used");
    if (!memory_port_db_.load(cfg.get_str("memory_port_db_path", "db/memory_port_db.yml")))
        LOG_ERROR("GameServer: memory_port_db failed to load — the Memory Ports cannot be used");
    if (!mon_speech_db_.load(cfg.get_str("mon_speech_db_path", "db/mon_speech_db.yml")))
        LOG_ERROR("GameServer: mon_speech_db failed to load — the monsters say nothing");
    if (!pet_speech_db_.load(cfg.get_str("pet_param_db_path", "db/pet_param_db.yml"),
                             cfg.get_str("pet_speech_db_path", "db/pet_speech_db.yml")))
        LOG_ERROR("GameServer: pet_speech_db failed to load — the pets say nothing");
    if (!teleport_adv_db_.load(cfg.get_str("teleport_adv_db_path", "db/teleport_adv_db.yml")))
        LOG_ERROR("GameServer: teleport_adv_db failed to load — the Portable Port AD cannot be used");
    if (!trans_scroll_db_.load(cfg.get_str("trans_scroll_db_path", "db/trans_scroll_db.yml")))
        LOG_ERROR("GameServer: trans_scroll_db failed to load — the disguise kits cannot be used");
    if (!post_setup_db_.load(cfg.get_str("post_setup_db_path", "db/post_setup_db.yml")))
        LOG_ERROR("GameServer: post_setup_db failed to load — the post box uses the original settings");
    if (!levelup_gift_db_.load(cfg.get_str("post_levelup_gift_db_path", "db/post_levelup_gift_db.yml")))
        LOG_ERROR("GameServer: post_levelup_gift_db failed to load — no level-up gifts");
    {
        // the post box's sync (retail dword_ADF858: minutes, the console "postbox_test" sets it; 0 -> 15)
        const int m = cfg.get_int("post_sync_minutes", 15);
        post_sync_ms_ = (m > 0 ? m : 15) * 60000;
    }
    // the megaphone queue's pace (MasterServer.ini MegaphoneDelay = 18000; 0 -> no broadcasts, retail)
    mega_delay_ms_ = cfg.get_int("megaphone_delay_ms", 18000);
    // the Master's world name in "[header] island - text" (its world table; not in the backup's data)
    mega_island_ = cfg.get_str("megaphone_island_name", "Paula");
    if (!debuff_resist_db_.load(cfg.get_str("debuff_resist_db_path", "db/monster_debuff_resist_db.yml")))
        LOG_ERROR("GameServer: monster_debuff_resist_db failed to load — the bosses resist no debuff");
    // The maps' NPC tables (NpcTalk_<area>): every map's NPCs, placed at the world build.
    if (!npc_db_.load(cfg.get_str("npc_db_path", "db/npc_db.yml")))
        LOG_ERROR("GameServer: npc_db failed to load — the maps get no NPCs");
    else
        spawn_world_entities();
    // Every zone with an ItemPercentage lays its buried items (MapItemManager, drill.cpp).
    build_map_items();
    // The NPC dialogs (NpcMsg_*) and the data of their script functions.
    if (!npc_msg_db_.load(cfg.get_str("npc_msg_db_path", "db/npc_msg_db.yml")))
        LOG_ERROR("GameServer: npc_msg_db failed to load — the NPCs have nothing to say");
    teleport_db_.load(cfg.get_str("teleport_db_path", "db/teleport_db.yml"));
    npc_move_db_.load(cfg.get_str("npc_move_db_path", "db/npc_move_db.yml"));
    npc_check_lv_msg_db_.load(cfg.get_str("npc_check_lv_msg_db_path", "db/npc_check_lv_msg_db.yml"));
    // The normal quests (QuestInfo, their QuestResult tables, RandomResultItemInfo; quest.cpp).
    if (!quest_db_.load(cfg.get_str("quest_db_path", "db/quest_db.yml"),
                        cfg.get_str("quest_result_db_path", "db/quest_result_db.yml"),
                        cfg.get_str("random_result_item_db_path", "db/random_result_item_db.yml"),
                        cfg.get_str("scenario_quest_db_path", "db/scenario_quest_db.yml"), item_db_))
        LOG_ERROR("GameServer: quest_db failed to load — the NPCs offer no quests");
    if (!quest_db_.load_monster(cfg.get_str("monster_quest_db_path", "db/monster_quest_db.yml"),
                                cfg.get_str("monster_quest_result_db_path", "db/monster_quest_result_db.yml"), item_db_))
        LOG_ERROR("GameServer: monster_quest_db failed to load — the NPCs offer no monster quests");
    if (!quest_db_.load_party(cfg.get_str("party_quest_db_path", "db/party_quest_db.yml"),
                              cfg.get_str("party_quest_result_db_path", "db/party_quest_result_db.yml"),
                              cfg.get_str("party_quest_exp_db_path", "db/party_quest_exp_db.yml"), item_db_))
        LOG_ERROR("GameServer: party_quest_db failed to load — the NPCs offer no party quests");
    if (!exchange_shop_db_.load(cfg.get_str("exchange_shop_db_path", "db/exchange_shop_db.yml"),
                                cfg.get_str("exchange_shop_table_db_path", "db/exchange_shop_table_db.yml")))
        LOG_ERROR("GameServer: exchange_shop_db failed to load — no exchange shops");
    if (!compound_db_.load(cfg.get_str("compound_db_path", "db/compound_db.yml"),
                           cfg.get_str("compounder_db_path", "db/compounder_db.yml")))
        LOG_ERROR("GameServer: compound_db failed to load — no compounds");
    {
        const std::string mode = cfg.get_str("restart_town", "last");
        restart_town_of_zone_ = (mode == "zone");
        LOG_INFO("GameServer: restart_town = %s", restart_town_of_zone_ ? "zone (town of the death zone)"
                                                                          : "last (last town visited, retail)");
    }

    uint16_t port = static_cast<uint16_t>(cfg.get_int("game_port", 8002));
    self_port_ = port;
    game_ip_   = cfg.get_str("game_ip", "127.0.0.1");   // address the client reaches us at
    // bind_ip: the local address both ports listen on (0.0.0.0 = every interface).
    std::string bind_ip = cfg.get_str("bind_ip", "0.0.0.0");
    if (bind_ip.empty()) bind_ip = "0.0.0.0";              // an empty value means the default, as in the other servers
    asio::error_code ec;
    const asio::ip::address_v4 bind_addr = asio::ip::make_address_v4(bind_ip, ec);
    if (ec) { LOG_FATAL("bind_ip '%s' is not an IPv4 address", bind_ip.c_str()); return false; }
    if (!open_acceptor(acceptor_, bind_addr, port, "game")) return false;

    LOG_INFO("cGameServer listening on %s:%u (announced as %s)", bind_ip.c_str(), port, game_ip_.c_str());
    if (game_ip_.rfind("127.", 0) == 0)
        LOG_WARN("cGameServer: game_ip is %s - only a client on this PC can follow a portal (S2C 154) or reach "
                 "the community port; set the server's public IPv4 in conf/game_server.conf", game_ip_.c_str());

    // The community port (the retail SystemServer's): the client opens a second connection to it,
    // at the address S2C 0x1585 gives (party, party board, ...). system_ip defaults to game_ip.
    system_port_ = static_cast<uint16_t>(cfg.get_int("system_port", port + 1));
    system_ip_   = cfg.get_str("system_ip", game_ip_);
    if (!open_acceptor(system_acceptor_, bind_addr, system_port_, "system")) return false;
    LOG_INFO("cGameServer: community (SystemServer) port %s:%u, announced as %s:%u", bind_ip.c_str(),
             system_port_, system_ip_.c_str(), system_port_);

    // MasterServer hub link
    master_ip_   = cfg.get_str("master_ip", "127.0.0.1");
    master_port_ = static_cast<uint16_t>(cfg.get_int("master_port", 9010));
    master_disp_.on(paula::inter::SVR_LOGIN_ACK_OK, [](paula::inter::InterLink&, int, paula::inter::StreamReader&) {
        LOG_INFO("cGameServer: Master handshake OK (link up)");
    });
    master_disp_.on(paula::inter::SVR_LOGIN_ACK_CANCEL, [](paula::inter::InterLink&, int, paula::inter::StreamReader&) {
        LOG_ERROR("cGameServer: Master rejected handshake");
    });
    connect_master();

    return true;
}

void cGameServer::connect_master()
{
    auto sock = std::make_shared<asio::ip::tcp::socket>(ioc_);
    asio::error_code aec;
    auto addr = asio::ip::make_address(master_ip_, aec);
    if (aec) { LOG_ERROR("cGameServer: bad master_ip '%s'", master_ip_.c_str()); return; }
    asio::ip::tcp::endpoint ep(addr, master_port_);

    sock->async_connect(ep, [this, sock](std::error_code ec) {
        if (ec) {
            LOG_ERROR("cGameServer: connect to Master %s:%u failed: %s (start MasterServer first)",
                      master_ip_.c_str(), master_port_, ec.message().c_str());
            return;
        }
        master_link_ = std::make_shared<paula::inter::InterLink>(std::move(*sock), master_disp_, 0);
        master_link_->start();
        paula::inter::StreamWriter w;
        w.str("GameServer");
        master_link_->send(paula::inter::SVR_LOGIN, w.data());
        LOG_INFO("cGameServer: connected to Master %s:%u, sent SVR_LOGIN", master_ip_.c_str(), master_port_);
    });
}

void cGameServer::run()
{
    accept_next();
    accept_system();
    start_heartbeat_loop();
    start_autosave();
    start_item_tick();
    megaphone_recover();                // the players' messages a stop left queued go back as items
    start_megaphone();
}

void cGameServer::save_all()
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    int n = 0;
    for (auto& kv : sessions_) {
        if (kv.second) { try { kv.second->save_now(); ++n; } catch (...) {} }
    }
    if (n) LOG_DEBUG("cGameServer: autosaved %d player(s)", n);   // (every 15 s: per-tick chatter)
}

void cGameServer::start_autosave()
{
    heartbeat_timer_.expires_after(std::chrono::seconds(15));
    heartbeat_timer_.async_wait([this](std::error_code ec) {
        if (ec) return;          // cancelled on stop
        save_all();
        start_autosave();        // reschedule
    });
}

void cGameServer::stop()
{
    asio::error_code ec;
    acceptor_.close(ec);
    system_acceptor_.close(ec);
    accept_retry_timer_.cancel();
    system_accept_retry_timer_.cancel();
    heartbeat_timer_.cancel();
    item_timer_.cancel();
    mega_timer_.cancel();
}

void cGameServer::broadcast_zone(int16_t zone_id, PacketBuffer& pkt, uint32_t exclude_session)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [sid, sess] : sessions_) {
        if (sid == exclude_session) continue;
        if (sess->zone_id() != zone_id) continue;
        sess->send(pkt);
    }
}

// The users of a zone (the MasterServer's zone record +208, MCMD_REQUEST_ZONE_USER_COUNT 3079 / 3080).
int cGameServer::zone_player_count(int16_t zone_id)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    int n = 0;
    for (auto& [sid, sess] : sessions_)
        if (sess->zone_id() == zone_id && sess->in_world()) ++n;
    return n;
}

uint16_t cGameServer::next_block_id()
{
    block_counter_ = (block_counter_ >= 0x7FFF) ? 1 : static_cast<uint16_t>(block_counter_ + 1);
    return block_counter_;
}

void cGameServer::broadcast_block(int16_t zone_id, std::vector<PacketBuffer>& group, uint32_t exclude_session)
{
    if (group.empty()) return;

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const uint16_t blk = next_block_id();   // under the lock: block_counter_ is not otherwise guarded
    for (auto& p : group) p.set_block_id(blk);

    for (auto& [sid, sess] : sessions_) {
        if (sid == exclude_session) continue;
        if (sess->zone_id() != zone_id) continue;
        // Send the whole block to THIS recipient contiguously (the session write-queue preserves
        // order), then the empty CLOSE (0x079F) with the same id — that is what makes the client
        // run the block and play the action from its first packet.
        for (auto& p : group) { PacketBuffer copy = p; sess->send(copy); }
        PacketBuffer close(static_cast<Opcode>(0x079F), sess->is_paula());
        close.set_block_id(blk);
        sess->send(close);
    }
}

void cGameServer::broadcast_block_split(int16_t zone_id, uint32_t self_session, std::vector<PacketBuffer>& mine,
                                        std::vector<PacketBuffer>& theirs)
{
    if (mine.empty() && theirs.empty()) return;
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const uint16_t blk = next_block_id();
    for (auto& p : mine)   p.set_block_id(blk);
    for (auto& p : theirs) p.set_block_id(blk);
    for (auto& [sid, sess] : sessions_) {
        if (sess->zone_id() != zone_id) continue;
        std::vector<PacketBuffer>& group = (sid == self_session) ? mine : theirs;
        if (group.empty()) continue;
        for (auto& p : group) { PacketBuffer copy = p; sess->send(copy); }
        PacketBuffer close(static_cast<Opcode>(0x079F), sess->is_paula());
        close.set_block_id(blk);
        sess->send(close);
    }
}

void cGameServer::player_entered(GameClientSession& who)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    PacketBuffer mine = who.build_appear();   // the newcomer, as seen by others
    for (auto& [sid, sess] : sessions_) {
        if (sid == who.session_id()) continue;
        if (sess->zone_id() != who.zone_id()) continue;
        if (sess->object_id() == 0) continue;            // not spawned yet
        // show the newcomer to this existing player
        PacketBuffer a = mine;  sess->send(a);
        // show this existing player to the newcomer
        PacketBuffer b = sess->build_appear();  who.send(b);
        // and its pet (the sector's objects, op 8 — pet.cpp)
        if (sess->has_pet()) { PacketBuffer pp = sess->build_pet_appear(0x0008); who.send(pp); }
        LOG_DEBUG("cGameServer: zone %d — %u <-> %u mutual appear", who.zone_id(),
                 who.object_id(), sess->object_id());
    }
    // Our own pet when it was made before the sector enter (a zone enter: sub_40C0F0 before EnterZone).
    if (who.has_pet()) { PacketBuffer pp = who.build_pet_appear(0x0008); who.send(pp); }
    // Show the world entities (monsters + NPCs) already in this zone to the entering player.
    appear_world_entities_to(who);
}

void cGameServer::player_left(int16_t zone_id, uint16_t eid, uint32_t exclude_session)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [sid, sess] : sessions_) {
        if (sid == exclude_session) continue;
        if (sess->zone_id() != zone_id) continue;
        // The logout's leave, S2C 6 [u16 id] (sub_5F76A0, from the Shells' LogoutPlayer). VERIFIED: client
        // sub_6F0960 cases 6 / 21 read the id, skip their own, play "dung jang" + the "etc_warp" effect on
        // the actor and remove it (vtbl+32). (0x000F, sub_5F7610, removes without the effect.)
        PacketBuffer p(static_cast<Opcode>(0x0006), sess->is_paula());
        p.write<uint16_t>(eid);
        sess->send(p);
    }
}

// ── World-entity (monster/NPC) appear serializers ───────────────────────────
// S2C op 8, SAME header as the player appear (reversed in GameClientSession::build_appear and
// confirmed against the client readers): [u16 eid][u16 kind][u16 species][u8 job][u8 flag]
// [u16 level][u32 stateFlags][coord1 x,y][coord2 x,y][cstr name]. kind = 3 (Monster) / 2 (NPC).
// Retail sub_715550: coord1 = the CURRENT position, coord2 = the destination (+120), stateFlags =
// actor+92 whose low 3 bits are the running move's type — the client walks a moving actor from
// coord1 to coord2 on appear (sub_499350 -> event 30016) instead of dropping it at the destination.
// Then a short class blob (client sub_6C35B0 monster / sub_6C53F0 npc), both starting with the
// 12-byte shared prefix sub_6BCE00: [u8 action][u8 facing][u8 moveSpeed][u32][u8 buffEnable=0][u32].
// moveSpeed -> client actor+120 = 4*b (step speed); 0 = static.
static void write_appear_header(PacketBuffer& p, uint16_t eid, uint16_t kind, uint16_t species,
                                uint8_t job, uint16_t level, int32_t cx, int32_t cy,
                                int32_t dx, int32_t dy, uint32_t state_flags, const std::string& name)
{
    p.write<uint16_t>(eid);
    p.write<uint16_t>(kind);
    p.write<uint16_t>(species);
    p.write<uint8_t>(job);
    p.write<uint8_t>(0);                               // flag
    p.write<uint16_t>(level);                          // level (actor+920)
    p.write<uint32_t>(state_flags);                    // stateFlags (actor+92)
    p.write<uint16_t>(static_cast<uint16_t>(cx)); p.write<uint16_t>(static_cast<uint16_t>(cy)); // coord1 = current
    p.write<uint16_t>(static_cast<uint16_t>(dx)); p.write<uint16_t>(static_cast<uint16_t>(dy)); // coord2 = destination
    const std::string nm = to_client_text(name);       // db UTF-8 -> the client's CP949
    p.write_bytes(nm.c_str(), nm.size() + 1);          // NUL-terminated name
}

static void write_shared_blob_prefix(PacketBuffer& p, uint8_t facing, uint8_t move_speed)
{
    p.write<uint8_t>(0);           // actionState
    p.write<uint8_t>(facing);      // facingDir (client: byte/256*2pi)
    p.write<uint8_t>(move_speed);  // moveSpeed -> actor+120 = 4*move_speed
    p.write<uint32_t>(0);          // actor+1200
    p.write<uint8_t>(0);           // buffEnable = 0 (no buffs)
    p.write<uint32_t>(0);          // discarded u32
}

// The monster's class blob (CommonMonster vtbl+48 sub_728AF0): the shared prefix, [u16 its owner's id (+1212),
// 0 without one][u8 +2028].
static PacketBuffer build_monster_appear(const MonsterEntity& m, bool paula, uint16_t opcode = 0x0008,
                                         uint16_t owner = 0)
{
    PacketBuffer p(static_cast<Opcode>(opcode), paula);
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    m.motion.pos(now, cx, cy);
    write_appear_header(p, static_cast<uint16_t>(m.object_id), /*kind*/3, m.species,
                        /*job*/0, m.level, cx, cy, m.pos_x, m.pos_y, m.motion.state_flags(now), m.name);
    write_shared_blob_prefix(p, static_cast<uint8_t>(m.facing), m.move_speed);
    p.write<uint16_t>(owner);      // its owner (sub_4269F0 / sub_4269D0), 0 = none
    p.write<uint8_t>(0);           // +2028 (actor+1208)
    return p;
}

// The NPC's class blob (Npc vtbl+48 sub_71D970): the shared prefix, its record's PosId (u16, client
// +1416) and its head mark (+1780 = the record's HeadMarkType, sub_71D720; client NPC_Head.nri).
// A shop (ShopNpc, kind 5) writes only the shared prefix (ShopNpc vtbl+48 = Character sub_715670);
// the client's ShopNpc (vtable 0xDDD71C) reads it with the shared reader sub_6BCE00. A skill master
// (SkillShopNpc, kind 14) adds its record's PosId (u16, vtbl+48 sub_720AB0); the client's SkillShopNpc
// (vtable 0xDDD8BC, made by sub_6EA900 case 14) reads only the shared prefix (sub_6BCE00).
static PacketBuffer build_npc_appear(const NpcEntity& n, bool paula)
{
    PacketBuffer p(static_cast<Opcode>(0x0008), paula);
    write_appear_header(p, static_cast<uint16_t>(n.object_id), /*kind*/n.shop ? 5 : (n.sshop ? 14 : 2), n.species,
                        /*job*/0, /*level*/1, n.pos_x, n.pos_y, n.pos_x, n.pos_y, /*flags*/0, n.name);
    write_shared_blob_prefix(p, static_cast<uint8_t>(n.facing), /*move_speed*/0);  // NPCs are static
    if (n.shop) return p;
    if (n.sshop) {
        p.write<uint16_t>(static_cast<uint16_t>(n.sshop->pos_id));
        return p;
    }
    int head_mark = 0;
    if (n.row) if (const std::string* v = n.row->get("HeadMarkType")) head_mark = std::atoi(v->c_str());
    p.write<uint16_t>(static_cast<uint16_t>(n.row ? n.row->pos_id : 0));
    p.write<uint32_t>(static_cast<uint32_t>(head_mark));
    return p;
}

NpcEntity* cGameServer::zone_npc(int16_t zone, uint16_t oid)
{
    NpcEntity* n = entity_mgr_.get_npc(oid);
    return (n && n->zone == zone && !n->shop && !n->sshop) ? n : nullptr;
}

// The zone's SkillShopManager lookup (zone +0xE2C -> sub_60E670): a skill master of the zone by id.
NpcEntity* cGameServer::zone_skill_shop(int16_t zone, uint16_t oid)
{
    NpcEntity* n = entity_mgr_.get_npc(oid);
    return (n && n->zone == zone && n->sshop) ? n : nullptr;
}

// The zone's ShopManager lookup (zone +0x5E4 -> sub_60E670): a ShopNpc of the zone by id.
NpcEntity* cGameServer::zone_shop(int16_t zone, uint16_t oid)
{
    NpcEntity* n = entity_mgr_.get_npc(oid);
    return (n && n->zone == zone && n->shop) ? n : nullptr;
}

void cGameServer::appear_world_entities_to(GameClientSession& who)
{
    const bool paula = who.is_paula();
    for (uint32_t oid : entity_mgr_.world_entities_in_zone(who.zone_id())) {
        const MonsterEntity* m = entity_mgr_.get_monster(oid);
        const NpcEntity*     n = m ? nullptr : entity_mgr_.get_npc(oid);
        if (!m && !n) continue;
        if (m && m->banished) continue;                      // its layer -1: no one sees it
        PacketBuffer p = m ? build_monster_appear(*m, paula, 0x0008, live_owner(*m)) : build_npc_appear(*n, paula);
        who.send(p);
    }
    appear_ground_items_to(who);
}

// A monster's owner as its RefT holds it: a player gone from the world is none.
uint16_t cGameServer::live_owner(const MonsterEntity& m)
{
    if (!m.owner_oid || !entity_mgr_.get_player(m.owner_oid)) return 0;
    return static_cast<uint16_t>(m.owner_oid);
}

void cGameServer::broadcast_entity_spawn(uint32_t oid, uint16_t opcode)
{
    const bool paula = janus::PaulaCrypt::Instance().IsLoaded();
    if (const MonsterEntity* m = entity_mgr_.get_monster(oid)) {
        PacketBuffer p = build_monster_appear(*m, paula, opcode, live_owner(*m));
        broadcast_zone(m->zone, p, 0);
    } else if (const NpcEntity* n = entity_mgr_.get_npc(oid)) {
        PacketBuffer p = build_npc_appear(*n, paula);
        broadcast_zone(n->zone, p, 0);
    }
}

// The world build's NPCs (NpcManager::Init sub_85DDC0 for every zone): each row of a map's
// NpcTable becomes an NPC (object kind 2) — sub_85DE20: Type must be a CharacterInfo id in
// 1000..1449 or 17000..479999 ("NpcManager::CreateNpc - invalid chrtype"), its position the zone's
// type-4 point of id PosId (sub_810EA0; none -> "not found NPC Point" and (600, 600)), its name the
// CharacterInfo name of Type (sub_622CD0). (Npc time tables — a timed NPC, +220 — and the ShopTable
// / SkillShopTable NPCs are not done yet.)
void cGameServer::spawn_world_entities()
{
    size_t made = 0, no_point = 0;
    for (const auto& [id, md] : maps_db_.all()) {
        if (md.npc_table.empty()) continue;
        const std::vector<NpcRow>* rows = npc_db_.table(md.npc_table);
        if (!rows) continue;
        const MapGrid* g = zone_grid(static_cast<int16_t>(id));
        for (const NpcRow& r : *rows) {
            if ((r.type < 1000 || r.type >= 1450) && (r.type < 17000 || r.type >= 480000)) {
                LOG_INFO("NpcManager::CreateNpc - invalid chrtype %d (map %d)", r.type, id);
                continue;
            }
            NpcEntity n;
            n.zone = static_cast<int16_t>(id);
            n.pos_x = 600; n.pos_y = 600;
            auto pt = g ? g->npc4.find(static_cast<uint32_t>(r.pos_id)) : decltype(g->npc4.end()){};
            if (g && pt != g->npc4.end()) {
                n.pos_x = pt->second.x; n.pos_y = pt->second.y;
            } else {
                ++no_point;
                LOG_INFO("NpcManager::CreateNpc - not found NPC Point, ZoneId:%d, FileName:%s, PointId:%d", id,
                         md.file.c_str(), r.pos_id);
            }
            n.species = static_cast<uint16_t>(r.type);
            n.npc_id  = static_cast<uint32_t>(r.type);
            n.name    = r.name;
            n.row     = &r;                       // the NPC's record (+1680): dialogs, scripts
            entity_mgr_.spawn_npc(n);
            ++made;
        }
    }
    LOG_INFO("NpcManager: %zu NPCs placed (%zu without their point)", made, no_point);
    spawn_shops();
}

// ShopManager::Init (sub_882C90) for every zone: each row of the map's ShopTable becomes a ShopNpc
// (object kind 5) — CreateShop sub_882CF0: its position the zone's type-5 point of id PosId
// (sub_810E40; none -> "ShopManager::CreateShop: can't find ShopPosId" and (600, 600)), its model
// NpcType, its name the CharacterInfo name of NpcType (sub_622CD0), its record at +1636.
void cGameServer::spawn_shops()
{
    size_t made = 0, no_point = 0;
    for (const auto& [id, md] : maps_db_.all()) {
        if (md.shop_table.empty()) continue;
        const std::vector<ShopRow>* rows = shop_db_.table(md.shop_table);
        if (!rows) continue;
        const MapGrid* g = zone_grid(static_cast<int16_t>(id));
        for (const ShopRow& r : *rows) {
            NpcEntity n;
            n.zone = static_cast<int16_t>(id);
            n.pos_x = 600; n.pos_y = 600;
            auto pt = g ? g->shop5.find(static_cast<uint32_t>(r.pos_id)) : decltype(g->shop5.end()){};
            if (g && pt != g->shop5.end()) {
                n.pos_x = pt->second.x; n.pos_y = pt->second.y;
            } else {
                ++no_point;
                LOG_INFO("ShopManager::CreateShop: can't find ShopPosId:%d (map %d)", r.pos_id, id);
            }
            n.species = static_cast<uint16_t>(r.npc_type);
            n.npc_id  = static_cast<uint32_t>(r.npc_type);
            n.name    = r.name;
            n.shop    = &r;
            entity_mgr_.spawn_npc(n);
            ++made;
        }
    }
    LOG_INFO("ShopManager: %zu shops placed (%zu without their point)", made, no_point);
    spawn_skill_shops();
}

// SkillShopManager::Init for every zone: each row of the map's SkillShopTable becomes a SkillShopNpc
// (object kind 14) — CreateSkillShop sub_883130: its position the zone's type-10 point of id PosId
// (sub_810F60; none -> (600, 600)), its model Type, its name the CharacterInfo name of Type
// (sub_622CD0), its record at +1800.
void cGameServer::spawn_skill_shops()
{
    size_t made = 0, no_point = 0;
    for (const auto& [id, md] : maps_db_.all()) {
        if (md.skill_shop_table.empty()) continue;
        const std::vector<SkillShopRow>* rows = skill_shop_db_.table(md.skill_shop_table);
        if (!rows) continue;
        const MapGrid* g = zone_grid(static_cast<int16_t>(id));
        for (const SkillShopRow& r : *rows) {
            NpcEntity n;
            n.zone = static_cast<int16_t>(id);
            n.pos_x = 600; n.pos_y = 600;
            auto pt = g ? g->skill10.find(static_cast<uint32_t>(r.pos_id)) : decltype(g->skill10.end()){};
            if (g && pt != g->skill10.end()) {
                n.pos_x = pt->second.x; n.pos_y = pt->second.y;
            } else {
                ++no_point;
            }
            n.species = static_cast<uint16_t>(r.type);
            n.npc_id  = static_cast<uint32_t>(r.type);
            n.name    = r.name;
            n.sshop   = &r;
            entity_mgr_.spawn_npc(n);
            ++made;
        }
    }
    LOG_INFO("SkillShopManager: %zu skill masters placed (%zu without their point)", made, no_point);
}

// One listening port. reuse_address stays off: on Windows it is SO_REUSEADDR, which lets a second copy of the
// server bind the same port without an error and split the connections between the two.
bool cGameServer::open_acceptor(asio::ip::tcp::acceptor& a, const asio::ip::address& ip, uint16_t port, const char* what)
{
    const asio::ip::tcp::endpoint ep(ip, port);
    asio::error_code ec;
    a.open(ep.protocol(), ec);
    if (ec) { LOG_FATAL("%s acceptor: %s", what, ec.message().c_str()); return false; }
    a.set_option(asio::ip::tcp::acceptor::reuse_address(false), ec);
    a.bind(ep, ec);
    if (ec) { LOG_FATAL("%s bind %s:%u: %s", what, ip.to_string().c_str(), port, ec.message().c_str()); return false; }
    a.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) { LOG_FATAL("%s listen: %s", what, ec.message().c_str()); return false; }
    return true;
}

// The handlers given to Session::start take the session from their argument: a lambda holding the session
// (the old [sess] capture) was stored in that same session, so it owned itself and was never freed (every
// connection and every field change leaked one). An accept error other than our own close (stop) does not end
// the accepts for good: listen again after a short pause (e.g. no socket handle left for the moment).
void cGameServer::accept_next()
{
    acceptor_.async_accept(
        [this](asio::error_code ec, asio::ip::tcp::socket socket) {
            if (ec) {
                if (ec == asio::error::operation_aborted) return;   // stop(): the acceptor closed
                LOG_ERROR("game accept: %s", ec.message().c_str());
                accept_retry_timer_.expires_after(std::chrono::milliseconds(250));
                accept_retry_timer_.async_wait([this](std::error_code tec) {
                    if (!tec && acceptor_.is_open()) accept_next();
                });
                return;
            }

            uint32_t id = next_session_id_++;
            auto sess = std::make_shared<GameClientSession>(std::move(socket), *this);
            sess->set_session_id(id);
            sess->set_paula(janus::PaulaCrypt::Instance().IsLoaded());
            sess->set_peer_ip(sess->remote_address());
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                sessions_[id] = sess;
            }
            LOG_INFO("cGameServer: connection [%u] from %s", id, sess->peer_ip().c_str());

            sess->start(
                [](std::shared_ptr<Session> s, PacketBuffer pkt) {
                    std::static_pointer_cast<GameClientSession>(s)->on_packet(s, std::move(pkt));
                },
                [this](std::shared_ptr<Session> s) { on_session_close(s); }
            );
            sess->begin_handshake();
            accept_next();
        });
}

// The community connections (system_session.cpp). Their ids come from the same counter as the game
// sessions', so one id names one connection. (Handlers and accept errors as in accept_next.)
void cGameServer::accept_system()
{
    system_acceptor_.async_accept(
        [this](asio::error_code ec, asio::ip::tcp::socket socket) {
            if (ec) {
                if (ec == asio::error::operation_aborted) return;   // stop(): the acceptor closed
                LOG_ERROR("system accept: %s", ec.message().c_str());
                system_accept_retry_timer_.expires_after(std::chrono::milliseconds(250));
                system_accept_retry_timer_.async_wait([this](std::error_code tec) {
                    if (!tec && system_acceptor_.is_open()) accept_system();
                });
                return;
            }
            const uint32_t id = next_session_id_++;
            auto sess = std::make_shared<SystemClientSession>(std::move(socket), *this);
            sess->set_session_id(id);
            sess->set_paula(janus::PaulaCrypt::Instance().IsLoaded());
            {
                std::lock_guard<std::mutex> lock(system_mutex_);
                system_sessions_[id] = sess;
            }
            LOG_INFO("cGameServer: community connection [%u] from %s", id, sess->remote_address().c_str());
            sess->start(
                [](std::shared_ptr<Session> s, PacketBuffer pkt) {
                    std::static_pointer_cast<SystemClientSession>(s)->on_packet(std::move(pkt));
                },
                [this](std::shared_ptr<Session> s) {
                    auto ss = std::static_pointer_cast<SystemClientSession>(s);
                    ss->on_closed();
                    std::lock_guard<std::mutex> lock(system_mutex_);
                    system_sessions_.erase(ss->session_id());
                });
            accept_system();
        });
}

uint32_t cGameServer::system_ticket(uint32_t char_uid)
{
    static thread_local std::mt19937 rng{ std::random_device{}() };
    uint32_t t = rng();
    if (!t) t = 1;
    std::lock_guard<std::mutex> lock(system_mutex_);
    system_tickets_[char_uid] = t;
    return t;
}

bool cGameServer::check_system_ticket(uint32_t char_uid, uint32_t ticket)
{
    std::lock_guard<std::mutex> lock(system_mutex_);
    auto it = system_tickets_.find(char_uid);
    return it != system_tickets_.end() && it->second == ticket;
}

void cGameServer::send_to_system(uint32_t sid, const PacketBuffer& pkt)
{
    std::lock_guard<std::mutex> lock(system_mutex_);
    auto it = system_sessions_.find(sid);
    if (it != system_sessions_.end() && it->second) it->second->send(pkt);
}

void cGameServer::on_session_close(std::shared_ptr<Session> s)
{
    // (the party and the session's own teardown first, outside the sessions lock: the party sends to
    // sessions under its own lock, on_close broadcasts to the zone)
    auto gs = std::static_pointer_cast<GameClientSession>(s);
    gs->party_closed();
    gs->on_close(s);
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(s->session_id());
}

void cGameServer::send_to_session(uint32_t sid, const PacketBuffer& pkt)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(sid);
    if (it != sessions_.end() && it->second) it->second->send(pkt);
}

void cGameServer::post_to_session(uint32_t sid, std::function<void(GameClientSession&)> fn)
{
    std::shared_ptr<GameClientSession> s;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(sid);
        if (it != sessions_.end()) s = it->second;
    }
    if (s) s->run_on_strand([s, fn = std::move(fn)]() { fn(*s); });
}

void cGameServer::start_heartbeat_loop()
{
    // Server-initiated 0x076C is a Disconnect command to the client (not a keepalive); send nothing.
    (void)heartbeat_timer_;
}

std::shared_ptr<GameClientSession> cGameServer::session_of_char(uint32_t char_uid)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::shared_ptr<GameClientSession> found;
    for (auto& kv : sessions_) {
        const auto& s = kv.second;
        if (!s || s->char_uid() != char_uid || !s->in_world_or_leaving()) continue;
        if (!found || (found->leaving() && !s->leaving())) found = s;
    }
    return found;
}

// A double login (release spec, GSC-04): a ticket-valid C2S 0x0004 / 156 for a character another connection
// still holds. Each session keeps its own money / EXP and saves them (logout, autosave), so two of them would
// roll each other back: the old one is saved and leaves the world NOW (its logout, synchronously: the save
// lands before the new session loads the character), then its connection closes (its own close later finds it
// Closed). A field change's old connection (154 sent) is not one: the new connection takes over from it.
// Runs on the new session's strand: with io_threads 1 (the release setting) every strand runs on the one io
// thread, so the old session's teardown cannot race its own handlers.
void cGameServer::kick_char(uint32_t char_uid, uint32_t except_sid, const std::string& from_ip, bool community)
{
    std::vector<std::shared_ptr<GameClientSession>> old;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& kv : sessions_) {
            const auto& s = kv.second;
            if (kv.first != except_sid && s && s->char_uid() == char_uid && s->holds_char() && !s->relocated())
                old.push_back(s);
        }
    }
    for (auto& s : old) {
        LOG_INFO("cGameServer: char %u logged in again from %s - its session %u (%s) is saved and closed",
                 char_uid, from_ip.c_str(), s->session_id(), s->peer_ip().c_str());
        on_session_close(s);
        s->close();
    }
    if (!community || old.empty()) return;
    // The old client's community links go with it (those from the new login's address may be the new
    // client's own: kept).
    std::vector<std::shared_ptr<SystemClientSession>> links;
    {
        std::lock_guard<std::mutex> lock(system_mutex_);
        for (auto& kv : system_sessions_)
            if (kv.second && kv.second->char_uid() == char_uid && kv.second->remote_address() != from_ip)
                links.push_back(kv.second);
    }
    for (auto& l : links) l->close();
}

bool cGameServer::char_online_from(uint32_t char_uid, const std::string& ip)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& kv : sessions_) {
        const auto& s = kv.second;
        if (s && s->char_uid() == char_uid && s->holds_char() && s->peer_ip() == ip) return true;
    }
    return false;
}

// The session of a player object id (the entity's session id -> the session), nullptr when none.
GameClientSession* cGameServer::player_session(uint32_t oid)
{
    PlayerEntity* p = entity_mgr_.get_player(oid);
    if (!p) return nullptr;
    const uint32_t sid = p->session_id;
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(sid);
    return (it != sessions_.end() && it->second) ? it->second.get() : nullptr;
}
