#include "entity_manager.hpp"
#include "logger.hpp"

EntityManager::EntityManager()
{
    id_used_.assign(0x10000, false);
    id_used_[0] = true;                                 // sub_7F3B10(…, 1): id 0 is taken at once
    for (uint32_t id = 1; id < 0x10000; ++id) free_ids_.push_back(static_cast<uint16_t>(id));
}

// IdManager::NewId (sub_411270 -> sub_411510): the head of the free list.
uint32_t EntityManager::new_id_locked()
{
    if (free_ids_.empty()) {
        LOG_ERROR("IdManager::NewId() - ID Over!!, maxCount:%d", 0x10000);
        return INVALID_OID;
    }
    const uint16_t id = free_ids_.front();
    free_ids_.pop_front();
    id_used_[id] = true;
    return id;
}

// IdManager::DropId (sub_7F3B80 -> sub_7F3D80): back at the tail of the free list.
void EntityManager::drop_id_locked(uint32_t oid)
{
    if (oid == INVALID_OID || oid >= 0x10000 || !id_used_[oid]) return;
    id_used_[oid] = false;
    free_ids_.push_back(static_cast<uint16_t>(oid));
}

uint32_t EntityManager::spawn_player(PlayerEntity ent)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Stable per-character oid: the Trickster client keeps its self-avatar (and its entityId)
    // across a seamless field change (the S2C 154 handler reuses *(GameWorld+1384) instead of
    // rebuilding it), so the same character MUST keep the same in-world id across a warp/reconnect,
    // or our move-echo carries an id the client no longer recognises as itself and it "walks in
    // place". Reuse the recorded oid for this char_uid; only allocate a fresh one the first time.
    uint32_t oid;
    if (ent.char_uid != 0) {
        auto it = char_oid_.find(ent.char_uid);
        if (it != char_oid_.end()) oid = it->second;
        else { oid = new_id_locked(); char_oid_[ent.char_uid] = oid; }
    } else {
        oid = new_id_locked();
    }
    ent.object_id = oid;
    players_[oid] = std::move(ent);
    return oid;
}

uint32_t EntityManager::spawn_monster(MonsterEntity ent)
{
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t oid = new_id_locked();
    ent.object_id = oid;
    ent.type = EntityType::Monster;
    ent.spawn_seq = ++spawn_seq_;
    monsters_[oid] = std::move(ent);
    return oid;
}

uint32_t EntityManager::spawn_npc(NpcEntity ent)
{
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t oid = new_id_locked();
    ent.object_id = oid;
    ent.type = EntityType::NPC;
    npcs_[oid] = std::move(ent);
    return oid;
}

void EntityManager::despawn(uint32_t oid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // A player keeps its id across a seamless field change (char_oid_), so only the other objects
    // give theirs back — and a player object that is not a character's stable id (it would be lost).
    size_t gone = 0;
    auto pit = players_.find(oid);
    if (pit != players_.end()) {
        const auto cit = char_oid_.find(pit->second.char_uid);
        if (cit == char_oid_.end() || cit->second != oid) ++gone;
        players_.erase(pit);
    }
    auto mit = monsters_.find(oid);
    if (mit != monsters_.end())
        for (const MonsterEntity::Es& e : mit->second.es) drop_id_locked(e.id);   // its effect states go too
    gone += monsters_.erase(oid) + npcs_.erase(oid) + items_.erase(oid);
    if (gone) drop_id_locked(oid);
}

uint32_t EntityManager::spawn_ground_item(GroundItemEntity ent)
{
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t oid = new_id_locked();
    ent.object_id = oid;
    ent.type = EntityType::Item;
    items_[oid] = std::move(ent);
    return oid;
}

std::vector<GroundItemEntity> EntityManager::ground_items_in_zone(int16_t zone_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<GroundItemEntity> result;
    for (auto& [oid, it] : items_)
        if (it.zone == zone_id) result.push_back(it);
    return result;
}

bool EntityManager::get_ground_item(uint32_t oid, GroundItemEntity& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = items_.find(oid);
    if (it == items_.end()) return false;
    out = it->second;
    return true;
}

bool EntityManager::take_ground_item(uint32_t oid, uint16_t count, bool& removed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    removed = false;
    auto it = items_.find(oid);
    if (it == items_.end()) return false;
    if (count >= it->second.count) { items_.erase(it); removed = true; drop_id_locked(oid); }
    else                           it->second.count = static_cast<uint16_t>(it->second.count - count);
    return true;
}

void EntityManager::tick_ground_items(int32_t elapsed_ms, std::vector<GroundItemEntity>& destroyed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = items_.begin(); it != items_.end(); ) {
        GroundItemEntity& g = it->second;
        g.timer_ms += elapsed_ms;
        if (g.owned) {
            // state 1: past 30000 ms -> state 0 (leaving state 1 clears the owner, event 3)
            if (g.timer_ms > 30000) { g.owned = false; g.owner_oid = 0; g.timer_ms = 0; }
            ++it;
            continue;
        }
        // state 0: a gelder pile lasts 60000 ms, any other item 90000 ms (then vtbl+28 destroy)
        const int32_t life = (g.item_type == 30) ? 60000 : 90000;
        if (g.timer_ms > life) { destroyed.push_back(g); drop_id_locked(it->first); it = items_.erase(it); }
        else ++it;
    }
}

bool EntityManager::despawn_player_if_owner(uint32_t oid, uint32_t session_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(oid);
    if (it == players_.end()) return false;
    if (it->second.session_id != session_id) return false;  // a newer session owns it now
    players_.erase(it);
    return true;
}

PlayerEntity* EntityManager::get_player(uint32_t oid)
{
    auto it = players_.find(oid);
    return it != players_.end() ? &it->second : nullptr;
}

MonsterEntity* EntityManager::get_monster(uint32_t oid)
{
    auto it = monsters_.find(oid);
    return it != monsters_.end() ? &it->second : nullptr;
}

NpcEntity* EntityManager::get_npc(uint32_t oid)
{
    auto it = npcs_.find(oid);
    return it != npcs_.end() ? &it->second : nullptr;
}

BaseEntity* EntityManager::get_entity(uint32_t oid)
{
    if (auto* p = get_player(oid))  return p;
    if (auto* m = get_monster(oid)) return m;
    if (auto* n = get_npc(oid))     return n;
    return nullptr;
}

bool EntityManager::damage_monster(uint32_t oid, int32_t amount, int32_t& out_hp, int32_t& out_max)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = monsters_.find(oid);
    if (it == monsters_.end()) { out_hp = 0; out_max = 0; return false; }
    MonsterEntity& m = it->second;
    bool was_alive = m.alive && m.hp > 0;
    m.hp -= amount;
    if (m.hp < 0) m.hp = 0;
    out_hp = m.hp; out_max = m.hp_max;
    bool killed = was_alive && m.hp == 0;
    if (killed) m.alive = false;
    return killed;
}

std::vector<uint32_t> EntityManager::entities_in_zone(int16_t zone_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint32_t> result;
    for (auto& [oid, p] : players_)
        if (p.zone == zone_id) result.push_back(oid);
    for (auto& [oid, m] : monsters_)
        if (m.zone == zone_id) result.push_back(oid);
    for (auto& [oid, n] : npcs_)
        if (n.zone == zone_id) result.push_back(oid);
    return result;
}

std::vector<uint32_t> EntityManager::players_in_zone(int16_t zone_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint32_t> result;
    for (auto& [oid, p] : players_)
        if (p.zone == zone_id) result.push_back(oid);
    return result;
}

std::vector<uint32_t> EntityManager::world_entities_in_zone(int16_t zone_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint32_t> result;
    for (auto& [oid, m] : monsters_)
        if (m.zone == zone_id) result.push_back(oid);
    for (auto& [oid, n] : npcs_)
        if (n.zone == zone_id) result.push_back(oid);
    return result;
}
