#pragma once
#include <cstring>
#include <string>
#include "store/char_record.hpp"
#include "interserver/byte_stream.hpp"

// Pack/unpack a CharRecord over the inter-server bus (Login <-> Master relay).
namespace paula {

inline void pack_char(inter::StreamWriter& w, const CharRecord& c)
{
    w.u32(c.uid);  w.u32(c.owner);  w.u8(c.slot);
    w.str(std::string(c.name));
    w.u16(static_cast<uint16_t>(c.level));
    w.u16(static_cast<uint16_t>(c.type));   w.u16(static_cast<uint16_t>(c.job));
    w.u16(static_cast<uint16_t>(c.type_2)); w.u8(c.job_2);
    w.u16(static_cast<uint16_t>(c.type_3)); w.u8(c.job_3);
    w.u8(c.f_type); w.u8(c.hair);
    w.u8(c.pts_p);  w.u8(c.pts_m);  w.u8(c.pts_s);  w.u8(c.pts_c);
    w.u8(c.state);  w.u8(c.authority);
    w.u16(static_cast<uint16_t>(c.zone));
    w.u32(static_cast<uint32_t>(c.pos_x)); w.u32(static_cast<uint32_t>(c.pos_y));
    w.u16(static_cast<uint16_t>(c.home));  w.u32(static_cast<uint32_t>(c.money));
    w.u8(c.deleted); w.str(std::string(c.freeze_until));
    w.u8(c.hair_2); w.u8(c.hair_3); w.u8(c.job_state);
    w.u8(c.tutorial_step); w.u8(c.guild_state); w.u8(c.is_guild_chief);
    w.u8(c.hack_index); w.u8(c.awakening_flag); w.u32(c.couple_code);
    w.u16(static_cast<uint16_t>(c.tm_level));
    for (uint32_t id : c.equip) w.u32(id);
}

inline CharRecord unpack_char(inter::StreamReader& r)
{
    CharRecord c{};
    c.uid = r.u32(); c.owner = r.u32(); c.slot = r.u8();
    { std::string n = r.str(); std::strncpy(c.name, n.c_str(), 17); }
    c.level = static_cast<int16_t>(r.u16());
    c.type  = static_cast<int16_t>(r.u16()); c.job = static_cast<int16_t>(r.u16());
    c.type_2 = static_cast<int16_t>(r.u16()); c.job_2 = r.u8();
    c.type_3 = static_cast<int16_t>(r.u16()); c.job_3 = r.u8();
    c.f_type = r.u8(); c.hair = r.u8();
    c.pts_p = r.u8(); c.pts_m = r.u8(); c.pts_s = r.u8(); c.pts_c = r.u8();
    c.state = r.u8(); c.authority = r.u8();
    c.zone  = static_cast<int16_t>(r.u16());
    c.pos_x = static_cast<int32_t>(r.u32()); c.pos_y = static_cast<int32_t>(r.u32());
    c.home  = static_cast<int16_t>(r.u16()); c.money = static_cast<int32_t>(r.u32());
    c.deleted = r.u8();
    { std::string f = r.str(); std::strncpy(c.freeze_until, f.c_str(), 19); }
    c.hair_2 = r.u8(); c.hair_3 = r.u8(); c.job_state = r.u8();
    c.tutorial_step = r.u8(); c.guild_state = r.u8(); c.is_guild_chief = r.u8();
    c.hack_index = r.u8(); c.awakening_flag = r.u8(); c.couple_code = r.u32();
    c.tm_level = static_cast<int16_t>(r.u16());
    for (uint32_t& id : c.equip) id = r.u32();
    return c;
}

} // namespace paula
