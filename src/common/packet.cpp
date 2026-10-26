#include "packet.hpp"
#include "opcodes.hpp"

// Opcode name lookup — useful for debug logging
const char* opcode_name(Opcode op)
{
    switch (op)
    {
    case Opcode::LOGIN_RESPONSE:        return "LOGIN_RESPONSE";
    case Opcode::HEARTBEAT_A:           return "HEARTBEAT_A";
    case Opcode::HEARTBEAT_B:           return "HEARTBEAT_B";
    case Opcode::HEARTBEAT_ACK:         return "HEARTBEAT_ACK";
    case Opcode::CONNECTION_CTRL:       return "CONNECTION_CTRL";
    case Opcode::SESSION_ENCRYPT:       return "SESSION_ENCRYPT";
    case Opcode::C2S_SESSION_READY:     return "C2S_SESSION_READY";
    case Opcode::SESSION_OPEN:          return "SESSION_OPEN";
    case Opcode::SESSION_CLOSE:         return "SESSION_CLOSE";
    case Opcode::C2S_SESSION_ACK:       return "C2S_SESSION_ACK";
    case Opcode::SESSION_PARAM_1:       return "SESSION_PARAM_1";
    case Opcode::SESSION_PARAM_2:       return "SESSION_PARAM_2";
    case Opcode::LOGIN_RESPONSE_2:      return "LOGIN_RESPONSE_2";
    case Opcode::SYSTEM_NOTIFY:         return "SYSTEM_NOTIFY";
    case Opcode::SERVER_BROADCAST:      return "SERVER_BROADCAST";
    case Opcode::CHAR_INFO:             return "CHAR_INFO";
    case Opcode::PARTY_APPLY_CONFIRM:   return "PARTY_APPLY_CONFIRM";
    case Opcode::CHAR_UPDATE:           return "CHAR_UPDATE";
    case Opcode::CHAR_POSITION:         return "CHAR_POSITION";
    case Opcode::ENTITY_LIST_UPDATE:    return "ENTITY_LIST_UPDATE";
    case Opcode::ENTITY_STATUS_SET:     return "ENTITY_STATUS_SET";
    case Opcode::SKILL_ACTIVATED:       return "SKILL_ACTIVATED";
    case Opcode::SKILL_RESULT:          return "SKILL_RESULT";
    case Opcode::DAMAGE_APPLY:          return "DAMAGE_APPLY";
    case Opcode::BUFF_APPLY:            return "BUFF_APPLY";
    case Opcode::DEBUFF_APPLY:          return "DEBUFF_APPLY";
    case Opcode::HEAL_APPLY:            return "HEAL_APPLY";
    case Opcode::CAST_START:            return "CAST_START";
    case Opcode::CAST_CANCEL:           return "CAST_CANCEL";
    case Opcode::CAST_COMPLETE:         return "CAST_COMPLETE";
    case Opcode::DRILL_INIT:            return "DRILL_INIT";
    case Opcode::DRILL_START:           return "DRILL_START";
    case Opcode::DRILL_RESULT:          return "DRILL_RESULT";
    case Opcode::C2S_DRILL_DISPATCH:    return "C2S_DRILL_DISPATCH";
    case Opcode::SOCIAL_INIT:           return "SOCIAL_INIT";
    case Opcode::COUPLE_LINK_ESTABLISH: return "COUPLE_LINK_ESTABLISH";
    case Opcode::ANGEL_PLAYER_ENTER:    return "ANGEL_PLAYER_ENTER";
    case Opcode::C2S_NPC_INTERACT:      return "C2S_NPC_INTERACT";
    case Opcode::C2S_QUEST_ACCEPT:      return "C2S_QUEST_ACCEPT";
    case Opcode::C2S_QUEST_COMPLETE:    return "C2S_QUEST_COMPLETE";
    case Opcode::C2S_ZONE_LOAD:         return "C2S_ZONE_LOAD";
    case Opcode::C2S_ZONE_ENTER:        return "C2S_ZONE_ENTER";
    case Opcode::C2S_FIELD_ENTER:       return "C2S_FIELD_ENTER";
    case Opcode::C2S_FIELD_POSITION_UPD:return "C2S_FIELD_POSITION_UPD";
    case Opcode::C2S_SKILL_USE:         return "C2S_SKILL_USE";
    case Opcode::C2S_ITEM_STATUS:       return "C2S_ITEM_STATUS";
    case Opcode::HANDSHAKE_PAULA:       return "HANDSHAKE_PAULA";
    case Opcode::PAULA_LOGIN_RESULT:    return "PAULA_LOGIN_RESULT";
    case Opcode::PAULA_LOGIN_FAIL:      return "PAULA_LOGIN_FAIL";
    case Opcode::C2S_SERVER_SELECT:     return "C2S_SERVER_SELECT";
    case Opcode::PAULA_CHANNEL_INFO:    return "PAULA_CHANNEL_INFO";
    case Opcode::PAULA_RESERVE_WORLD_NAK: return "PAULA_RESERVE_WORLD_NAK";
    case Opcode::PAULA_SERVER_LIST:     return "PAULA_SERVER_LIST";
    default:                            return "UNKNOWN";
    }
}
