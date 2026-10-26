#pragma once
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
//  Trickster Online — Packet Opcodes
//  Reverse engineered from Trickster.bin (PolarTrickster / KTO)
//  Direction: S2C = server→client, C2S = client→server, BOTH = bidirectional
// ─────────────────────────────────────────────────────────────────────────────

enum class Opcode : uint16_t
{
    // ── 0. Paula Protocol (KTO Season 2) ────────────────────────────────────
    HANDSHAKE_PAULA         = 0x2CED,   // C2S  Login credentials
    PAULA_LOGIN_RESULT      = 0x2CEE,   // S2C  CTrLoginUserAck 11502: u32 + u64 auth + u8 n + n×75 island rows (n=2 → two-world window, no names)
    PAULA_LOGIN_FAIL        = 0x2CEF,   // S2C  Login failure
    C2S_SERVER_SELECT       = 0x2CF1,   // C2S  CTrReserveWorldReq 11505: u16 world + u16 island no
    PAULA_CHANNEL_INFO      = 0x2CF2,   // S2C  CTrReserveWorldAck 11506: that island's IP(16)+port(u16) = 18 bytes
    PAULA_RESERVE_WORLD_NAK = 0x2CF3,   // S2C  CTrReserveWorldNak 11507: u32 error code
    PAULA_SERVER_LIST       = 0x07D2,   // S2C  Server/world list (login); Char list (char server)
    C2S_CHAR_AUTH           = 0x07D0,   // C2S  Char server auth: skip(8)+account_id(u32)+token(u32)+5b
    CHAR_AUTH_ACK           = 0x07EC,   // S2C  Char server auth ack (sent before char list)
    C2S_CHAR_CREATE         = 0x07D6,   // C2S  Create character
    CHAR_CREATE_OK          = 0x07D8,   // S2C  Char create success (full entry)
    CHAR_CREATE_FAIL        = 0x07D7,   // S2C  Char create fail: u32 error_code
    C2S_CHAR_DELETE         = 0x07D9,   // C2S  Delete character request (2009): i64 char_uid + flag + str.
    CHAR_DELETE_OK          = 0x07DB,   // S2C  Delete success (2011, LCMD_DEL_CHR_ACK_OK) — EMPTY body.
                                        //      Real LoginServer sends this; client case 2011 rebuilds the
                                        //      char list (sub_747F50), which removes the card. Confirmed in
                                        //      recovered LoginServer sub_406D30: sub_403440(idx, 2011, 0, 0).
    CHAR_DELETE_FAIL        = 0x07DA,   // S2C  Delete failure (2010, LCMD_DEL_CHR_ACK_CANCEL) — u32 error
                                        //      (1025 no char, 1026 charserver failure, 20 generic).
    C2S_CHAR_SELECT         = 0x07DC,   // C2S  Select character: u32 char_id
    CHAR_SELECT_MAP         = 0x07DE,   // S2C  Map server info for selected char
    CHAR_SELECT_OK          = 0x0003,   // S2C  Final "connect now" after char select
    C2S_SLOT_QUERY          = 0x0817,   // C2S  Query slot expansion ticket count
    SLOT_TICKET_COUNT       = 0x0818,   // S2C  Slot expansion ticket count: u32
    C2S_SLOT_EXPAND         = 0x0814,   // C2S  Expand character slots
    SLOT_EXPANDED           = 0x0815,   // S2C  New slot count after expansion: u32
    C2S_CAPSULE_QUERY       = 0x07F6,   // C2S  Query time capsule ticket count
    C2S_CAPSULE_LIST        = 0x07F8,   // C2S  List frozen characters in time capsule

    // ── Map Server Auth ──────────────────────────────────────────────────────
    C2S_MAP_AUTH            = 0x0004,   // C2S  First packet to map server: char_uid_lo(4)+char_uid_hi(4)+0(4)+zone(2) = 14 bytes
    MAP_ENTER_ACK           = 0x0005,   // S2C  Map entry confirmed: u8 (0=ok) — triggers NewCharSelectTask→NewGameTask transition
    C2S_MOVE                = 0x0018,   // C2S  Move request (bot movement.md): [u16 srcX][u16 srcY][u16 dstX][u16 dstY][u8 moveType]
    C2S_GUIDE_REQ           = 0x04A5,   // C2S  guide/history request (client sub_5A2290 on enter); server replies 0x04A6
    C2S_UNK_0194            = 0x0194,   // C2S  post-enter request (404); server-side internal per GameServer

    // ── 1. Control / System ──────────────────────────────────────────────────
    LOGIN_RESPONSE          = 0x0321,   // S2C  Login/system response
    HEARTBEAT_A             = 0x076C,   // BOTH Heartbeat control A
    HEARTBEAT_B             = 0x076D,   // BOTH Heartbeat control B
    HEARTBEAT_ACK           = 0x07EC,   // S2C  Keep-alive acknowledgment
    CONNECTION_CTRL         = 0x07ED,   // S2C  Connection control
    SESSION_ENCRYPT         = 0x07F2,   // S2C  Encryption / session init
    C2S_SESSION_READY       = 0x1581,   // C2S  Before SessionOpen
    SESSION_OPEN            = 0x1582,   // S2C  Session established
    SESSION_CLOSE           = 0x1583,   // S2C  Session closed
    C2S_SESSION_ACK         = 0x1584,   // C2S  Session acknowledge
    SESSION_PARAM_1         = 0x1585,   // S2C  Session parameter 1
    SESSION_PARAM_2         = 0x1586,   // S2C  Session parameter 2
    C2S_SESSION_PARAM       = 0x1587,   // C2S  Session parameter
    LOGIN_RESPONSE_2        = 0x15C6,   // S2C  Login/system response alt
    SYSTEM_NOTIFY           = 0x15D1,   // S2C  System notification
    SERVER_PATCH            = 0x15DB,   // S2C  Server patch/update notice
    SERVER_BROADCAST        = 0xFDEB,   // S2C  Server broadcast message

    C2S_SYSTEM_INIT         = 0x1500,   // C2S  System initialization
    C2S_SYSTEM_QUERY        = 0x1510,   // C2S  Sent from 3 locations
    C2S_SYSTEM_ACTION       = 0x1514,   // C2S  --

    // ── 2. GameCore — Chat / Party ───────────────────────────────────────────
    PARTY_DATA_UPDATE       = 0x1773,   // S2C  Party data full update
    REQUEST_ACTION          = 0x1774,   // C2S  Entity action request
    C2S_CHAT                = 0x0036,   // C2S  In-world map chat. CONFIRMED (live capture): payload at
                                        //      offset 9 (after the 9-byte Paula header) = NUL-terminated
                                        //      ASCII text, no prefix (text_len = size - 10). '@' => atcommand.
    CHANNEL_CHAT            = 0x1777,   // C2S  Channel chat message
    PARTY_ACTION            = 0x177A,   // C2S  Party action (single byte)
    ZONE_CHAT_SEND          = 0x177D,   // C2S  Zone chat send
    PARTY_INVITE_SEND       = 0x17A9,   // C2S  Party invite request
    PARTY_INVITE_WITH_NAME  = 0x17AC,   // C2S  Party invite with name + bool

    // ── 3a. Character / Party — Primary ─────────────────────────────────────
    CHAR_INFO               = 0x1B5A,   // S2C  name(36B)+coords+attrs
    PARTY_CREATE_FAIL       = 0x1B5B,   // S2C
    PARTY_DIVIDE_EXP_FAIL   = 0x1B5E,   // S2C
    CHAR_REFRESH            = 0x1B60,   // S2C  No packet read, pure logic
    PARTY_CHANGE_NAME_FAIL  = 0x1B61,   // S2C
    PARTY_SET_AUTO_ACCEPT   = 0x1B64,   // S2C  bool(1 byte)
    PARTY_MEMBER_DETAIL     = 0x1B66,   // S2C  Compound multi-field
    CHAR_UPDATE             = 0x1B67,   // S2C  dword + byte
    PARTY_APPLY_CONFIRM     = 0x1B68,   // S2C  VERIFIED in client: s2c_char_7016 -> ui_party_apply_confirm_dialog
                                        //      ("Invite to Party"). Was mislabeled CHAR_STAT — NOT a stat packet.
    PARTY_FORCE_UPDATE      = 0x1B6A,   // S2C
    CHAR_FLAG_SET           = 0x1B6B,   // S2C  byte
    PARTY_MEMBER_UPDATE     = 0x1B6D,   // S2C  stride=0x4C member array
    CHAR_TRIGGER            = 0x1B72,   // S2C  No params
    CHAR_FLAG_SET_2         = 0x1B73,   // S2C  byte
    PARTY_INVITE_RESULT     = 0x1B78,   // S2C
    PARTY_DATA_A            = 0x1B79,   // S2C  word × 2
    PARTY_DATA_B            = 0x1B7A,   // S2C  word × 2
    PARTY_SLOT              = 0x1B7B,   // S2C  word
    PARTY_MEMBER_EXP        = 0x1B7C,   // S2C  dword
    PARTY_LOOT_TYPE         = 0x1B7D,   // S2C  byte + conditional item list
    PARTY_MEMBER_STAT_A     = 0x1B7E,   // S2C  word + byte
    PARTY_MEMBER_DATA       = 0x1B7F,   // S2C  word + dword
    PARTY_INVITE_RESULT_2   = 0x1B82,   // S2C  word + byte + party desc
    PARTY_MEMBER_STAT_B     = 0x1B83,   // S2C  word + dword
    PARTY_FORCE_NORMAL_MODE = 0x1B84,   // S2C  word + qword + word
    PARTY_GROUP_UPDATE      = 0x1B85,   // S2C  word × 2
    PARTY_MEMBER_UID_A      = 0x1B87,   // S2C  qword (64-bit ID)
    PARTY_MEMBER_UID_B      = 0x1B88,   // S2C  qword (64-bit ID)
    PARTY_MEMBER_LOCATION   = 0x1B89,   // S2C  qword + dword + word + byte
    PARTY_MEMBER_STAT_UPD   = 0x1B8A,   // S2C  word×2
    PARTY_MEMBER_EXP_2      = 0x1B8B,   // S2C
    PARTY_BREAK             = 0x1B8C,   // S2C  Alert_BreakParty
    PARTY_UNEXPECTED_BREAK  = 0x1B8D,   // S2C  Alert_UnexpectedBreak
    ITEM_SHARE_FAIL         = 0x1B8E,   // S2C
    ITEM_SHARE_MODE         = 0x1B8F,   // S2C  byte
    CHAR_VALUE_64_A         = 0x1B94,   // S2C  word + qword
    CHAR_VALUE_64_A2        = 0x1B95,   // S2C  word + qword
    CHAR_VALUE_64_B         = 0x1B96,   // S2C
    CHAR_VALUE_64_B2        = 0x1B97,   // S2C
    CHAR_POSITION           = 0x1B98,   // S2C  dword + word + dword
    PARTY_REQUEST           = 0x1B9A,   // S2C
    PARTY_DENIED            = 0x1B9D,   // S2C  Alert_Denied
    PARTY_MEMBER_FULL       = 0x1BA1,   // S2C  word + dword×2 + word + byte×2
    PARTY_FLAG              = 0x1BA3,   // S2C  bool
    PARTY_MEMBER_FULL_2     = 0x1BA6,   // S2C
    PARTY_INFO_INVALID      = 0x1BA7,   // S2C  Alert_InvalidPartyInfo
    PARTY_REQUEST_CANCEL    = 0x1BA8,   // S2C  Alert_RequestCanceled
    PARTY_SETUP_MEMBER      = 0x1BA9,   // S2C  ReadWord + member setup
    PARTY_UPDATE_MEMBER     = 0x1BAA,   // S2C  ReadWord + ReadByte
    PARTY_MEMBER_LEVEL      = 0x1BAB,   // S2C  ReadByte + float conv
    PARTY_MEMBER_STATS      = 0x1BAC,   // S2C  ReadWord + ReadDword

    C2S_CHAR_ACTION_69      = 0x1B69,   // C2S
    C2S_CHAR_ACTION_A3      = 0x1BA3,   // C2S
    C2S_CHAR_BATCH          = 0x1BCC,   // C2S
    C2S_CHAR_QUERY          = 0x1C20,   // C2S

    // ── 3b. Character / Party — Extended ────────────────────────────────────
    PARTY_INFO_REQUEST      = 0x1C85,   // S2C
    PARTY_INFO_UPDATE       = 0x1C87,   // S2C
    PARTY_INFO_INVITE       = 0x1C8A,   // S2C
    PARTY_BOARD_ACCEPT      = 0x1C8D,   // S2C  Alert_BoardWritten
    PARTY_BOARD_REJECT      = 0x1C8E,   // S2C
    PARTY_BOARD_ACCEPT_2    = 0x1C90,   // S2C
    PARTY_BOARD_REJECT_2    = 0x1C91,   // S2C
    PARTY_STATE_CHANGE      = 0x1C93,   // S2C
    PARTY_STATE_CHANGE_2    = 0x1C96,   // S2C
    PARTY_DETAILED_INFO     = 0x1C9B,   // S2C
    PARTY_FAILURE_ALERT     = 0x1C9C,   // S2C
    PARTY_MEMBER_INFO       = 0x1C9E,   // S2C
    PARTY_MEMBER_ERROR      = 0x1C9F,   // S2C

    C2S_PARTY_EXT_86        = 0x1C86,
    C2S_PARTY_EXT_89        = 0x1C89,
    C2S_PARTY_EXT_8C        = 0x1C8C,
    C2S_PARTY_EXT_8F        = 0x1C8F,
    C2S_PARTY_EXT_92        = 0x1C92,
    C2S_PARTY_EXT_95        = 0x1C95,
    C2S_PARTY_EXT_98        = 0x1C98,
    C2S_PARTY_EXT_99        = 0x1C99,
    C2S_PARTY_EXT_9A        = 0x1C9A,
    C2S_PARTY_EXT_9D        = 0x1C9D,
    C2S_PARTY_EXT_B9        = 0x1CB9,

    // ── 4. Entity Management (Module 7500) ───────────────────────────────────
    // NOTE: IDA confirms dispatcher range starts at 0x1D4D; 0x1D4C is unhandled.
    // ENTITY_LIST_UPDATE is therefore 0x1D4D (first entry in the handled range).
    ENTITY_LIST_UPDATE      = 0x1D4D,   // S2C  u16 count + entity loop  (was 0x1D4C — wrong)
    ENTITY_NOTIFY           = 0x1D4E,   // S2C  No payload                (shifted +1)
    ENTITY_REQUEST          = 0x1D50,   // C2S  4 subtypes (byte 1-4)
    ENTITY_SUB_DISPATCH     = 0x1D51,   // S2C  u8 sub-opcode (0/1/2)
    ENTITY_NOTIFY_2         = 0x1D52,   // S2C
    ENTITY_BULK_FIELD       = 0x1D55,   // S2C  u8 count + loop setter
    ENTITY_STATUS_SET       = 0x1D56,   // S2C  u16 objectID + u8 status
    ENTITY_ACK              = 0x1D5A,   // S2C  Reply to ENTITY_REQUEST
    ENTITY_TRIGGER          = 0x1D5B,   // S2C  u16 → enum dispatch
    ENTITY_COMPLEX_LIST     = 0x1D5E,   // S2C  3×u8 header + 5-tuple loop
    ENTITY_COMPUTED_REPLY   = 0x1D5F,   // S2C  x*3 calculated reply
    ENTITY_SLOT_LIST        = 0x1D62,   // S2C  Max 26 entries

    // ── 5. Item / Equipment (Module 8000) ────────────────────────────────────
    ITEM_STATUS_CHANGE      = 0x1F43,   // S2C
    ITEM_DWORD_PARAM        = 0x1F45,   // S2C
    ITEM_BYTE_PARAM         = 0x1F46,   // S2C
    ITEM_CONDITIONAL_CHECK  = 0x1F47,   // S2C
    ITEM_SIMPLE_ACTION      = 0x1F48,   // S2C
    ITEM_MULTI_WORD_READ    = 0x1F4A,   // S2C
    ITEM_BYTE_LOGIC         = 0x1F4B,   // S2C
    ITEM_COMPLEX_STRUCT     = 0x1F4E,   // S2C
    ITEM_WORD_SELECT        = 0x1F4F,   // S2C
    ITEM_BYTE_DWORD_PAIR    = 0x1F50,   // S2C
    ITEM_NO_PARAM_ACTION    = 0x1F52,   // S2C
    ITEM_BYTE_FLAG          = 0x1F53,   // S2C
    ITEM_LARGE_STRUCT_A     = 0x1F54,   // S2C
    ITEM_BYTE_ACTION_A      = 0x1F56,   // S2C
    ITEM_BYTE_ACTION_B      = 0x1F57,   // S2C
    ITEM_BYTE_ACTION_C      = 0x1F58,   // S2C
    ITEM_ENTITY_STRUCT_A    = 0x1F5A,   // S2C
    ITEM_BYTE_ACTION_D      = 0x1F5B,   // S2C
    ITEM_ENTITY_STRUCT_B    = 0x1F5C,   // S2C
    ITEM_DUAL_STRUCT        = 0x1F5D,   // S2C
    ITEM_LARGE_DATA_PARSE   = 0x1F5E,   // S2C
    ITEM_STATE_SET          = 0x1F5F,   // S2C
    ITEM_POSITION_UPDATE    = 0x1F60,   // S2C
    ITEM_MOVEMENT_ACTION    = 0x1F62,   // S2C
    ITEM_BYTE_ACTION_E      = 0x1F63,   // S2C
    ITEM_ANIMATION          = 0x1F64,   // S2C
    ITEM_MESSAGE_BROADCAST  = 0x1F66,   // S2C
    ITEM_BYTE_ACTION_F      = 0x1F67,   // S2C
    ITEM_COMPLEX_CONDITIONAL= 0x1F68,   // S2C
    ITEM_SPECIAL_ACTION     = 0x1F69,   // S2C
    ITEM_NO_PARAM_B         = 0x1F6A,   // S2C
    ITEM_WORD_ACTION        = 0x1F6B,   // S2C
    ITEM_BYTE_ACTION_G      = 0x1F6D,   // S2C
    ITEM_NO_PARAM_C         = 0x1F6E,   // S2C
    ITEM_BYTE_ACTION_H      = 0x1F6F,   // S2C
    ITEM_BYTE_ACTION_I      = 0x1F71,   // S2C
    ITEM_BYTE_ACTION_J      = 0x1F72,   // S2C
    ITEM_CONDITIONAL_BYTE   = 0x1F73,   // S2C

    C2S_ITEM_STATUS         = 0x1F41,
    C2S_ITEM_STATUS_2       = 0x1F44,
    C2S_ITEM_SIMPLE         = 0x1F49,
    C2S_ITEM_RECT           = 0x1F4C,
    C2S_ITEM_DATA           = 0x1F4D,
    C2S_ITEM_BATCH_UPDATE   = 0x1F51,
    C2S_ITEM_SIMPLE_2       = 0x1F59,
    C2S_ITEM_RECT_2         = 0x1F61,
    C2S_ITEM_STATUS_3       = 0x1F65,
    C2S_ITEM_SIMPLE_3       = 0x1F6C,
    C2S_ITEM_RECT_3         = 0x1F75,

    // ── 6. Skill / Status Effect (Module 8500) ───────────────────────────────
    SKILL_ACTIVATED         = 0x213D,
    SKILL_RESULT            = 0x213E,
    DAMAGE_APPLY            = 0x213F,
    BUFF_APPLY              = 0x2140,
    BUFF_DURATION           = 0x2141,
    DEBUFF_APPLY            = 0x2142,
    DEBUFF_REMOVE           = 0x2143,
    STATUS_UPDATE           = 0x2144,
    SKILL_COOLDOWN          = 0x2146,
    BUFF_LIST_UPDATE        = 0x2148,   // was 0x214B → correction: 0x2148
    CAST_START              = 0x214B,
    CAST_CANCEL             = 0x214C,
    CAST_COMPLETE           = 0x214D,
    AOE_DAMAGE              = 0x214E,
    HEAL_APPLY              = 0x214F,
    RESURRECT_PERFORM       = 0x2151,
    STUN_APPLY              = 0x2152,
    SLOW_APPLY              = 0x2153,
    INCAPACITATE_APPLY      = 0x2154,
    BUFF_REFRESH            = 0x2155,
    COMBAT_ENTER            = 0x2156,
    PASSIVE_SKILL_GAIN      = 0x2159,
    SKILL_LEVEL_UP          = 0x215B,
    DEBUFF_IMMUNITY         = 0x215C,
    REFLECT_DAMAGE          = 0x215E,
    SKILL_RESULT_2          = 0x2160,
    DEBUFF_LIST_UPDATE      = 0x2161,
    PEACE_ZONE_EFFECT       = 0x2162,
    AGGRESSIVE_ZONE_EFFECT  = 0x2163,
    DAMAGE_APPLY_2          = 0x2164,
    ABSORB_SHIELD           = 0x2166,
    PROVOKE_EFFECT          = 0x2168,
    BLIND_EFFECT            = 0x2169,
    SILENCE_EFFECT          = 0x216A,
    POISON_EFFECT           = 0x216D,
    CURSE_EFFECT            = 0x216E,
    BLEAK_EFFECT            = 0x216F,
    WEAKNESS_EFFECT         = 0x2170,
    PARALYSIS_EFFECT        = 0x2172,
    CONFUSION_EFFECT        = 0x2173,
    FROZEN_EFFECT           = 0x2174,
    BURNING_EFFECT          = 0x2175,
    TRANSFORM_SKILL         = 0x2176,
    SUMMON_SPAWN            = 0x2177,
    SUMMON_DESPAWN          = 0x2178,
    CRITICAL_HIT            = 0x217C,
    MISS_HIT                = 0x217E,
    DODGE_AVOID             = 0x2180,
    BLOCK_ABSORB            = 0x2181,
    COUNTER_ATTACK          = 0x2183,
    BUFF_STACK_ADD          = 0x2184,
    DEBUFF_STACK_ADD        = 0x2185,
    ATTRIBUTE_MODIFIER      = 0x2186,
    SKILL_INTERRUPTED       = 0x2187,
    SKILL_INTERRUPTED_2     = 0x2188,
    SKILL_FAILURE           = 0x2189,
    SKILL_NOTIFY            = 0x218B,

    C2S_SKILL_USE           = 0x213A,
    C2S_SKILL_ACTION        = 0x215A,
    C2S_SKILL_SINGLE        = 0x215F,
    C2S_ADV_SKILL           = 0x217D,
    C2S_SKILL_TOGGLE        = 0x217F,
    C2S_SKILL_MULTI_PARAM   = 0x218A,

    // ── 7. Drilling / Detect (Module 10000) ──────────────────────────────────
    DRILL_INIT              = 0x2711,   // S2C
    DRILL_START             = 0x2712,   // S2C
    C2S_DRILL_INIT          = 0x2713,
    C2S_DRILL_ACTION        = 0x2714,
    C2S_DRILL_END           = 0x2715,
    DRILL_RESULT            = 0x2716,   // S2C
    DRILL_ITEM              = 0x2717,   // S2C
    DRILL_FIELD             = 0x2718,   // S2C
    DRILL_NOTIFY_09         = 0x2719,
    DRILL_NOTIFY_10         = 0x271A,
    DRILL_NOTIFY_11         = 0x271B,
    DRILL_NOTIFY_12         = 0x271C,
    DRILL_NOTIFY_13         = 0x271D,
    DRILL_NOTIFY_14         = 0x271E,
    DRILL_NOTIFY_15         = 0x271F,
    DRILL_NOTIFY_16         = 0x2720,
    DRILL_NOTIFY_17         = 0x2721,
    DRILL_NOTIFY_18         = 0x2722,
    DRILL_UPDATE_19         = 0x2723,
    DRILL_UPDATE_20         = 0x2724,
    DRILL_UPDATE_21         = 0x2725,
    DRILL_UPDATE_22         = 0x2726,
    DRILL_UPDATE_25         = 0x2729,

    C2S_DRILL_MASTER        = 0x2710,
    C2S_DRILL_DISPATCH      = 0x271C,   // Most-used C2S (35 locations)
    C2S_DRILL_SPECIAL       = 0x2728,

    // ── 8. Notification (Module 10500) ───────────────────────────────────────
    NOTIFY_A                = 0x2905,
    NOTIFY_B                = 0x2906,

    // ── 9. Social / Pairing (Module 11000) ───────────────────────────────────
    SOCIAL_INIT             = 0x2AFA,
    SOCIAL_UPDATE_ENTITY    = 0x2AFC,
    C2S_SOCIAL_REGISTER     = 0x2AFD,
    SOCIAL_UPDATE_PAIR_LIST = 0x2AFE,
    SOCIAL_SET_STATUS       = 0x2AFF,
    SOCIAL_SET_PAIR_DATE    = 0x2B00,
    SOCIAL_SET_PAIR_DATE_ALT= 0x2B01,
    C2S_SOCIAL_REQ_A        = 0x2B02,
    C2S_SOCIAL_REQ_B        = 0x2B03,
    C2S_SOCIAL_REQ_C        = 0x2B04,

    // ── 10. NPC / Quest / Map ────────────────────────────────────────────────
    C2S_NPC_INTERACT        = 0x2BFD,
    C2S_DIALOG_OPEN         = 0x2BB3,
    C2S_DIALOG_RESPONSE     = 0x2BCA,
    C2S_SHOP_BUY            = 0x2BC8,
    C2S_SHOP_OPEN           = 0x2BCC,
    C2S_QUEST_COMPLETE      = 0x2BAD,
    C2S_QUEST_ACCEPT        = 0x2BAF,
    C2S_QUEST_PROGRESS      = 0x2BB1,
    C2S_QUEST_LIST_QUERY    = 0x2BB5,
    C2S_QUEST_REWARD_SELECT = 0x2BBF,
    C2S_QUEST_ABANDON       = 0x2BC4,
    // Seamless intra-server field change / portal (reverse/specs/07 §2.7, confirmed [C]).
    // A same-server map move is NOT the login batch: server sends 154 RELOCATE, client replies
    // 156 CMD_ENTER_ZONE (light re-enter, NO spawn batch, NO 0x0135), server sends 437+157.
    S2C_RELOCATE            = 0x009A,   // 154  S2C -> moving client: {u32 token, u64 chrUid, cstr ip,
                                        //       u32 port, u32, u16 zoneTo, x:u16,y:u16 startPos, u8 type}
    C2S_ENTER_ZONE_CMD      = 0x009C,   // 156  C2S  client entering the new field (after loading it)
    S2C_ENTER_OK            = 0x009D,   // 157  S2C -> mover: {u32} enter confirmed
    S2C_ENTER_BCAST         = 0x01B5,   // 437  S2C  enter broadcast {u16 objId, x:u16,y:u16}
    C2S_ZONE_LOAD           = 0x2CD4,
    C2S_ZONE_ENTER          = 0x2CD7,
    C2S_EVENT_TRIGGER       = 0x2C92,
    C2S_EVENT_COMPLETE      = 0x2C9B,
    C2S_EVENT_FAIL          = 0x2CB2,
    C2S_EVENT_REWARD        = 0x2CB4,
    C2S_EVENT_STATUS        = 0x2CB6,
    C2S_FIELD_ENTER         = 0x2C89,
    C2S_FIELD_READY         = 0x2C8C,
    C2S_FIELD_LEAVE         = 0x2C95,
    C2S_FIELD_POSITION_UPD  = 0x2C98,
    C2S_FIELD_TELEPORT      = 0x2CAD,

    C2S_NPC_DIALOG_A        = 0x2B6E,
    C2S_NPC_DIALOG_B        = 0x2B72,
    C2S_NPC_SHOP_A          = 0x2B74,
    C2S_NPC_SHOP_B          = 0x2B78,
    C2S_NPC_SHOP_C          = 0x2B79,
    C2S_NPC_TRADE           = 0x2B7D,
    C2S_QUEST_ACTION_A      = 0x2B89,
    C2S_QUEST_ACTION_B      = 0x2BB7,
    C2S_QUEST_ACTION_C      = 0x2BB9,
    C2S_QUEST_ACTION_D      = 0x2BBB,
    C2S_QUEST_ACTION_E      = 0x2BC2,
    C2S_QUEST_DATA          = 0x2BCE,
    C2S_QUEST_COMPLETE_EXT  = 0x2BD0,
    C2S_QUEST_SPECIAL       = 0x2BF3,

    C2S_MAP_WARP_A          = 0x2C07,
    C2S_MAP_WARP_B          = 0x2C09,
    C2S_MAP_WARP_C          = 0x2C0B,
    C2S_MAP_WARP_D          = 0x2C0D,
    C2S_MAP_WARP_E          = 0x2C0F,
    C2S_MAP_EVENT_A         = 0x2C43,
    C2S_MAP_EVENT_B         = 0x2C47,
    C2S_MAP_INSTANCE_A      = 0x2C57,
    C2S_MAP_INSTANCE_B      = 0x2C59,
    C2S_MAP_INSTANCE_C      = 0x2C5C,
    C2S_MAP_INSTANCE_D      = 0x2C5D,

    // ── 11. Angel / Mentor System (Module 11800) ─────────────────────────────
    ANGEL_PLAYER_ENTER      = 0x2E19,
    C2S_ANGEL_REGISTER_CHAR = 0x2E1B,
    ANGEL_UPDATE_CHAR_VITALS= 0x2E1C,
    C2S_ANGEL_HELP_REQUEST  = 0x2E1D,
    ANGEL_HELP_REQ_SUCCESS  = 0x2E1E,
    ANGEL_HELP_REQ_FAIL     = 0x2E1F,
    ANGEL_SET_ONLINE_STATUS = 0x2E20,
    ANGEL_UPDATE_PAIR_XY    = 0x2E21,
    ANGEL_GRADUATION_MSG    = 0x2E23,
    ANGEL_ENTITY_POS_UPDATE = 0x2E24,
    C2S_ANGEL_HALL_ACTION   = 0x2E25,
    ANGEL_EXIT_HALL         = 0x2E26,
    ANGEL_HELP_ACCEPT_FAIL  = 0x2E28,
    ANGEL_PAIRING_STATUS    = 0x2E29,
    ANGEL_PAIRING_BY_ID     = 0x2E2A,
    C2S_ANGEL_REQ_2C        = 0x2E2C,
    C2S_ANGEL_REQ_2D        = 0x2E2D,

    // ── 12. Couple / Marriage System (Module 11900) ──────────────────────────
    COUPLE_POS_MOVING       = 0x2E82,
    COUPLE_POS_OFFLINE      = 0x2E83,
    COUPLE_LINK_ESTABLISH   = 0x2E8D,
    COUPLE_MAKE_HALL_MSG    = 0x2E8F,
    COUPLE_REQ_ACCEPT       = 0x2E91,
    COUPLE_REQ_RESET        = 0x2E93,
    COUPLE_LINK_BREAK       = 0x2E95,
    C2S_COUPLE_ACTION       = 0x2E96,
    COUPLE_INVITE_LIST      = 0x2E97,
    C2S_COUPLE_INVITE_REQ   = 0x2E98,
    COUPLE_REQ_STATUS_UPD   = 0x2E99,
    COUPLE_MEMBER_UPDATE_A  = 0x2E9A,
    COUPLE_MEMBER_UPDATE_B  = 0x2E9B,
    COUPLE_CONFIRM_PAIR     = 0x2E9C,
    COUPLE_INVITE_LIST_B    = 0x2E9E,
    COUPLE_DETAIL_INFO_A    = 0x2EA0,
    COUPLE_DETAIL_INFO_B    = 0x2EA2,
    COUPLE_TRIPLET_UPDATE   = 0x2EA4,
    COUPLE_SESSION_RESET    = 0x2EA6,
    C2S_COUPLE_REQ_A8       = 0x2EA8,
    C2S_COUPLE_REQ_AA       = 0x2EAA,
    COUPLE_STATUS_CHANGE    = 0x2EAB,
    COUPLE_DIVORCE_ASK      = 0x2EAD,
    COUPLE_PROPOSE          = 0x2EAF,
    COUPLE_STATUS_CHANGE_ALT= 0x2EB2,

    C2S_COUPLE_INIT         = 0x2E81,
    C2S_COUPLE_LINK_REQ     = 0x2E8E,
    C2S_COUPLE_ACCEPT       = 0x2E90,
    C2S_COUPLE_REJECT       = 0x2E92,
    C2S_COUPLE_BREAK_REQ    = 0x2E94,

    // ── Unknown / Reserved ───────────────────────────────────────────────────
    C2S_UNK5200_BASE        = 0x1478,
    C2S_UNK5200_ACTION_A    = 0x1488,
    C2S_UNK5200_ACTION_B    = 0x1489,
    C2S_UNK5200_ACTION_C    = 0x148A,
    C2S_UNK5200_ACTION_D    = 0x1491,
    C2S_UNK5200_QUERY       = 0x14F0,
    C2S_UNK5200_REQUEST     = 0x14F8,

    // ── 13. KR 2014 in-world spawn sequence (Zuri spec) — map server ─────────
    //  Sent by the map server after C2S_MAP_AUTH (0x0004). The KR client uses this
    //  low-opcode batch (NOT the 0x1Bxx/0x1Dxx range) to build the world + player.
    //  Phase 1 — char state + batch handshake (all required, in order):
    GAME_CHAR_DATA          = 0x02A7,   // S2C  char id, user id, slot, name, species, timestamp
    GAME_BATCH_HEADER       = 0x00E6,   // S2C  entityId mirror + spawn X/Y
    GAME_ENTITY_LIST_KR     = 0x00E7,   // S2C  item list — empty (count=0) ok for login
    GAME_CHAR_STATS         = 0x00E9,   // S2C  full stat block  (exact layout: TODO from Zuri)
    GAME_UNK_EA             = 0x00EA,   // S2C  empty list-count (00 00)
    GAME_MONSTER_QUEST      = 0x00EB,   // S2C  empty list-count
    GAME_SKILL_QUEST        = 0x0128,   // S2C  empty list-count
    GAME_SKILL_LIST_KR      = 0x010F,   // S2C  starter skill (1 entry)
    GAME_ITEM_EXPIRY        = 0x01FC,   // S2C  GameItemExpiryPacket default
    S2C_ACTIVE_QUEST_RESTORE= 0x0341,   // S2C  empty (no active quests)
    // GAME_PHASE_2 = MAP_ENTER_ACK (0x0005): KR-specific, exactly 1 byte 0x00
    GAME_ENTITY_ANNOUNCE_3D1= 0x03D1,   // S2C  entityId + 10 zero bytes
    GAME_BATCH_CONST_377    = 0x0377,   // S2C  KR float array — 40B/10 floats (Default0377Kr2014)
    GAME_BATCH_CONST_376    = 0x0376,   // S2C  KR 40B float array (Default0376Kr2014)
    GAME_BATCH_COUNTER_3D7  = 0x03D7,   // S2C  counter blob (counter=1, magic 0xD62E5000, u32=65, ts)
    GAME_BATCH_CONST_2A4    = 0x02A4,   // S2C  empty list-count
    GAME_BATCH_CONST_3E4    = 0x03E4,   // S2C  slot-count = 3
    GAME_ENTITY_ANNOUNCE_3E7= 0x03E7,   // S2C  entityId + 11 zero bytes
    GAME_BATCH_CONST_3EA    = 0x03EA,   // S2C  empty list-count
    //  Phase 2 — the actual spawn:
    // ANGEL_PAIRING_STATUS (0x2E29): angel-pairing = none
    PLAYER_SPAWN_LOGIN      = 0x0007,   // S2C  *** self-spawn — renders the player *** (hdr 22B + name + footer 71B)
    NPC_SPAWN               = 0x0008,   // S2C  NPC spawn loop (skip for login-only)
    GAME_UNK_135            = 0x0135,   // S2C  empty
    GAME_UNK_25C            = 0x025C,   // S2C  empty
    GAME_UNK_3A6            = 0x03A6,   // S2C  heading vector
    ENTITY_ID_ECHO          = 0x03A5,   // S2C  *** entityId — binds the spawned entity to "me" ***
    GAME_UNK_325            = 0x0325,   // S2C  color/flag
    S2C_HEALTH_STATUS       = 0x0024,   // S2C  current HP
    S2C_MANA_STATUS         = 0x0025,   // S2C  current MP
    GAME_UNK_477            = 0x0477,   // S2C  timestamped sparse
    S2C_MAP_HANDOFF_ACK_4CC = 0x04CC,   // S2C  map-handoff ack
    GAME_UNK_4A6            = 0x04A6,   // S2C  guide history
    S2C_ENTITY_SPEED        = 0x01D8,   // S2C  entityId + run speed

    // In-world map change / field load (RE'd in Client_live: dispatcher sub_6F0960
    // high-opcode table → sub_6EDB50 → sub_6E80C0(mapId) loads the field & sets the
    // zone). Payload = [u16 mapId][u16 posX][u16 posY] (ReadU16 + ReadU16x2). The
    // client's in-world camera/minimap derefs a NULL field if no map was ever loaded
    // (crash 0x6e212b), so this is what drives the player into an actual map.
    S2C_MAP_CHANGE          = 0x017C,   // S2C  load map: u16 mapId + u16 posX + u16 posY

    UNKNOWN                 = 0xFFFF,
};

// Helper: opcode name for logging
const char* opcode_name(Opcode op);
