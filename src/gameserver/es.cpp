#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include "paula_crypt.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>

// ─────────────────────────────────────────────────────────────────────────────
//  Effect states (ES) — 1:1 with the retail GameServer (GameServerRelease).
//
//  A skill whose ESAction has no handler of its own in the ESAction switch (sub_5BE9A0 returns 0)
//  goes through sub_524950: the ES is put on the caster (sub_524F20), or for the ESActions listed in
//  es_targets_object() on the selected target object (sub_524F60). sub_524690 then:
//    ESClass -1                         -> create it, no limiter;
//    SkillParam Type != 2, skill != 7046 -> the target's ESLimiter must admit it (vtbl+40,
//                                          sub_5E1CE0), create it, add it to the limiter (vtbl+44);
//    else                               -> create it, no limiter.
//  Create = zone sub_7FD4E0: the factory sub_527F90(ESAction) (one pooled class per ESAction), a new
//  object id from the global pool, ESAction::InitESAction sub_50F910 (the fields below, the apply
//  roll vtbl+112 sub_50F890), then the class's vtbl+116. Each zone tick runs vtbl+56 with the elapsed
//  ms; the end is vtbl+28 (sub_50F770): S2C 280 + the class's vtbl+100, the object is removed.
//
//  Packets (all VERIFIED in the client, Trickster.bin):
//    S2C 224 (0xE0), the first packet of a command block (the block runner sub_6EBF20 plays an
//      opener 224 through sub_4A8680 -> event 30098 -> the ESAction factory sub_55CD40):
//      [u16 caster][u16 target][u16 ES id][u16 ESAction][u8 2][u8 ESClass][u16 skill][u16 EState]
//      + the class's tail (sub_5EDE00; the client's class reads it in vtbl+132, e.g. the
//      ESAPriMarksManship sub_4C1D50 reads [u8]).
//    S2C 280 (0x118) [u16 target][u16 ES id] (sub_5EE170 / sub_5EE2A0 to the area, sub_5EE220 to
//      the target): the field router sub_6F0960 case 280 hands it to the target (sub_47B250), else
//      ends the ES by its id (sub_6F0050 -> vtbl+32).
//    S2C 283 (0x11B) [u8 n]{[u8 stat][u32 bonus]} (sub_5ED310) to the target: the Avatar
//      (sub_6B4E00 case 0x11B) stores each bonus at +556 + 4*stat — the buff part of its stats.
//    S2C 850 (0x352) [u16 ESAction] (sub_604360) to the target, once, when 10 s are left
//      (ESClass not 3 / -1): sub_6B4E00 case 0x352 finds the ES (sub_55F900) and sets +420 = 1.
//
//  (Below: some of the classes done. No one place lists them all: the factory switch in es_apply_skill maps
//  most skills' ESActions, and the rest are made by their own handler through es_create — in es.cpp and the
//  skill_*.cpp files, e.g. ESASelfCureDo (skill_power.cpp), ESAFluentWaist, ESAStatUpScrollDo. Grep es_create.)
//  The classes done (their vtbl+116 / +56 / +100 and their StatusValueAdder, ResChain 9):
//    1079 ESAAcUp, 1092 ESADaUp, 1095 ESAHVUp, 1104 ESAMagicWind, 1112 ESALuckUp, 1122
//      ESAMagicWall, 1118 ESAStaticShield — the row's Ratio (+4) and Seconds (+8); stat += (base +
//      items) * Ratio on AC / DA / HV / MA / LK / MD / DP (ESAStaticShield: tail [u16 Seconds][u8],
//      and no recompute at its start or end);
//    1101 ESAAgilityUp — the row's DX (int); DX += DX;
//    1115 ESAPriMarksManShip — the row's AP (int); AC += AP / 20.0 (its vtbl+116 tail is an
//      Enigma-redirected jump in the recovered EXE, jmp [0x1187160]; rebuilt from its restore
//      sub_553A40 / save sub_5539F0, which keep exactly +216 Seconds, +8, +212 AP, and the class
//      template every sibling's vtbl+116 follows);
//    1345 ESACustomizingGun — the row's Ratio and Seconds, no adder (the gun damage looks it up);
//    1476 ESAFastLoad — 1000 * Seconds ms and the float Ratio, kept although not applied, tail
//      [f32 Ratio] (the FastShot check looks it up);
//    1217 ESASharpSense — only when applied: Player +1064 = Ratio (the gun power shot's multiplier).
//    1391 ESABoiling ("끓는 냄비") — 1000 * Second ms; applied: the Crazy Drill gauge boost = StressUp
//      (CCrazyDrillMgr +64, sub_76A460), back to 0 at its end (sub_58F040).
//  The skill scrolls' classes (ESClass 4 / 5, cast by C2S 844, skill.cpp):
//    1301..1305 ESAApUpScroll / Ma / Lk / Hv / DaUpScroll (sub_57B380 ...) — the row's Value (+4, i16) and
//      Seconds (+6) (loader sub_51C2A0); stat += Value on AP / MA / LK / HV / DA, 1000 * Seconds ms; tail:
//      1301 none, 1302..1305 [u8 1] — a constant, not the apply result. (VERIFIED in the client: its
//      factory sub_55CD40 makes 1301 an ESAHeartBeatDo — reads nothing — and 1302..1305 ESAMagicWind /
//      ESALuckUp / ESAHVUp / ESADaUp — read [u8].)
//    1387 ESAStatUpScroll (sub_58D700, row loader sub_51D270: Seconds +4, ApplyRatio +8 — so the
//      apply roll's +0 is 0: always applied —, CalculateType +14, the 12 ratios +16.., 4 strings,
//      PartySkillType +576, Radius +578) — no packet: a 1 s countdown (+196) that, with PartySkillType 2,
//      collects the caster's share group within Radius (sub_58B9F0, the caster among them) and puts an
//      ESAction 1388 on the caster (and on each of them, each its own caster) through sub_524690(.., 0):
//      ESClass the same, EState 0, flags 2.
//      Its first base tick sends the 10 s warning S2C 850 [1387] (no lifetime, +188 = 0); its end S2C 280.
//    1388 ESAStatUpScrollDo (sub_58DB10) — the 12 bonuses: CalculateType 1 = (int)ratio; 2 = of the
//      target's level (+1020): AP / MP / MD / WT / HP / DP 4 * Lv * (int)ratio / 10, AC / MA / DA / LK / HV
//      Lv * (int)ratio / 40, DX 0; counted down from 1000 * Seconds; tail [u8 result][BeginEffect]
//      [ProcessEffect][BeginSound][ProcessSound] (C strings; VERIFIED: the client's ESAStatUpScrollDo
//      sub_516500 reads u8 + 4 pkt_read_str); its update (sub_58E210) recomputes and sends S2C 283 with
//      each stat that has a bonus — the buff (+656 + 4i), but MD's from +580: the stat itself (retail).
//  The ESLimiter's admission of the "debuff" classes 4 / 5 / 6 (sub_894280 — in this data only the
//  scrolls): an ES of the same SKILL in the list is replaced (or of the same ScrollGroup — no skill has
//  one), unless SkillOption 4096; then the capacity (list 4: 5) as for the others.
//  A made ES of those classes on a player's skill uses up the scroll at Player +0xC94 (sub_524690 with
//  a4 = 1, sub_8911A0 after an ESAction with its own handler): Player::UseItem -> sub_6E59C0, S2C 0x0059.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr int     kEsLists  = 8;
// PlayerESLimiter (sub_76CD50 -> sub_5E16A0(10, 5, 5, 5, 5, 5) -> sub_5E1320): the capacity of each
// ESClass list; classes 5 and 6 use list 4 (their own lists are 0).
constexpr int     kPlayerEsCapacity[kEsLists] = { 10, 5, 5, 5, 5, 0, 0, 5 };
constexpr int32_t kEsWarnMs = 10000;    // sub_50FF90: lifetime <= 10000 -> S2C 850 once
constexpr uint8_t kEsKind   = 2;        // ESAction +152 (the action struct +20 = 2, sub_7BEF40)

int es_list(int esclass) { return (esclass == 5 || esclass == 6) ? 4 : esclass; }

// sub_894280: the debuff classes.
bool es_debuff_class(int esclass) { return esclass == 4 || esclass == 5 || esclass == 6; }

// The skills (by (char) id, sub_763DD0 / sub_763E00) a skill-resist ES covers: ESAPreventSlowDo 2215 (sub_580A90),
// ESAInsulatorDo 4301 (sub_56EA20), ESAGasMaskDo 4010 (sub_57DBE0), ESAInsulateArmorDo 2113 and 5115 (sub_57E100).
std::vector<int> skill_resist_ids(int esaction)
{
    switch (esaction) {
    case 1328: return { 2215 };
    case 1270: return { 4301 };
    case 1313: return { 4010 };
    case 1314: return { 2113, 5115 };
    default:   return {};
    }
}

// sub_524950: the ESActions whose ES goes to the selected target object (sub_524F60); every other
// one goes to the caster (sub_524F20).
bool es_targets_object(int esaction)
{
    switch (esaction) {
    case 1081: case 1096: case 1098: case 1099: case 1102: case 1106: case 1107: case 1109: case 1110:
    case 1117: case 1119: case 1225: case 1226: case 1232: case 1236: case 1237: case 1273: case 1315:
    case 1316: case 1318: case 1320: case 1321: case 1322: case 1324: case 1332: case 1334: case 1355:
    case 1363: case 1382: case 1395: case 1396: case 1449: case 1474:
    case 1526: case 1537: case 1543:                       // the Bear's debuffs (skill_bear.cpp, a reconstruction)
        return true;
    default:
        return false;
    }
}

// The ESAction row loaders (sub_510690) that do not put ApplyRatio at the row's +0 — the constructor's
// 0.0 stays there (sub_51E2A0): 1387 / 1388 (sub_51D270, it goes to +8), 1391 (sub_51D450), 1397..1399
// (sub_51D4E0), 1423 (sub_51D520), 1424 (sub_51D570), 1425 (sub_51D5C0).
bool esa_row_has_apply_ratio(int esaction)
{
    switch (esaction) {
    case 1387: case 1388: case 1391: case 1397: case 1398: case 1399: case 1423: case 1424: case 1425:
    case 1357:                                             // (sub_51CDD0: its ApplyRatio at +16)
    case 1369: case 1371: case 1373: case 1375: case 1377: // (sub_51CF80: ApplyRatio at +16 — the gardens and
    case 1370: case 1372: case 1374: case 1376: case 1378: //   their Dos, which carry the garden's row)
    case 1360: case 1361: case 1379: case 1386:            // (sub_51CD70 / sub_51CD20 / sub_51D010: none)
    case 1380:                                             // (sub_51D070: its ApplyRatio at +12)
    case 1289:                                             // (sub_51C020: Seconds / Radius / Comment only)
    case 1382:                                             // (sub_51D170: its ApplyRatio at +12)
        return false;
    default:
        return true;
    }
}

// sub_8911A0's action flags: 2, | 16 for SkillOption 512 (left behind into a map of type 1 / 5 / 6),
// | 8 for SkillOption 1.
uint16_t es_flags(const SkillDef& d)
{
    return static_cast<uint16_t>(2 | ((d.skill_option & 512) ? 16 : 0) | ((d.skill_option & 1) ? 8 : 0));
}

} // namespace

// ── sub_524950 / sub_524690: the generic ESAction of a skill ─────────────────────────────
void GameClientSession::es_apply_skill(const SkillDef& d, uint8_t level, const EsaRow* act)
{
    // The factory (sub_527F90): the classes done so far.
    EsKind kind = EsKind::None;
    switch (d.esaction) {
    case 1079: case 1092: case 1095: case 1104: case 1112:
    case 1118: case 1122:                                  kind = EsKind::StatRatio;      break;
    case 1101:
    case 1301: case 1302: case 1303: case 1304: case 1305: kind = EsKind::StatValue;      break;
    case 1115:                                             kind = EsKind::PriMarksman;    break;
    case 1345:                                             kind = EsKind::CustomizingGun; break;
    case 1476:                                             kind = EsKind::FastLoad;       break;
    case 1217:                                             kind = EsKind::SharpSense;     break;
    case 1390:                                             kind = EsKind::ThirdEye;       break;
    case 1391:                                             kind = EsKind::Boiling;        break;
    case 1387:                                             kind = EsKind::ScrollShell;    break;
    case 1397: case 1398: case 1399:                       kind = EsKind::Marker;         break;
    case 1423:                                             kind = EsKind::Light;          break;
    case 1424:                                             kind = EsKind::WideHealing;    break;
    case 1425:                                             kind = EsKind::PartyDetect;    break;
    case 1223:                                             kind = EsKind::Anniversary;    break;
    case 1125: case 1127: case 1129: case 1210: case 1212:
    case 1152: case 1175:                                  kind = EsKind::Aura;           break;
    case 1146:                                             kind = EsKind::PowerThrow;     break;
    case 1286:                                             kind = EsKind::Hiding;         break;
    case 1192: case 1267:                                  kind = EsKind::Mine;           break;
    case 1289:                                             kind = EsKind::OpticalEyes;    break;
    case 1385:                                             kind = EsKind::SharpSenseEx;   break;
    case 1208: case 1248:                                  kind = EsKind::Mutation;       break;
    case 1245:                                             kind = EsKind::SharpClaw;      break;
    case 1311:                                             kind = EsKind::Subcutaneous;   break;
    case 1172:                                             kind = EsKind::WhiteKnight;    break;
    case 1479:                                             kind = EsKind::Kamikaze;       break;
    case 1280:                                             kind = EsKind::FullHouse;      break;
    case 1097:                                             kind = EsKind::MagicReflect;   break;
    case 1142:                                             kind = EsKind::Dash;           break;
    case 1309:                                             kind = EsKind::Guard;          break;
    case 1351:                                             kind = EsKind::DeadAlive;      break;
    case 1171:                                             kind = EsKind::Berserk;        break;
    case 1274:                                             kind = EsKind::StatRatio;      break;
    case 1275: case 1367:                                  kind = EsKind::FlashCut;       break;
    case 1368:                                             kind = EsKind::EnergySword;    break;
    case 1157:                                             kind = EsKind::ThunderBolt;    break;
    case 1161:                                             kind = EsKind::WindSlash;      break;
    case 1186:                                             kind = EsKind::WhirlWind;      break;
    case 1247: case 1250: case 1251: case 1252: case 1253: kind = EsKind::ElemShield;     break;
    case 1353: case 1358:                                  kind = EsKind::Field;          break;
    case 1357:                                             kind = EsKind::GaleBreak;      break;
    case 1337: case 1338: case 1339: case 1340: case 1341: kind = EsKind::Counter;        break;
    case 1369: case 1371: case 1373: case 1375: case 1377: kind = EsKind::ElemArea;       break;
    case 1154:                                             kind = EsKind::Bind;           break;
    case 1254:                                             kind = EsKind::DarkBarrier;    break;
    case 1255:                                             kind = EsKind::LightShield;    break;
    case 1333:                                             kind = EsKind::HolyShield;     break;
    case 1360:                                             kind = EsKind::BloodyOath;     break;
    case 1361:                                             kind = EsKind::SaintPromise;   break;
    case 1285:                                             kind = EsKind::SkyRay;         break;
    case 1379:                                             kind = EsKind::SaintOath;      break;
    case 1342:                                             kind = EsKind::DarkGhost;      break;
    case 1380:                                             kind = EsKind::DarkWhisper;    break;
    case 1524: case 1525: case 1527: case 1534: case 1540: case 1541:
    case 1542: case 1544:                                  kind = EsKind::BearStat;       break;   // (skill_bear.cpp)
    case 1547:                                             kind = EsKind::HammerWheel;    break;   // (skill_bear.cpp)
    case 1528:                                             kind = EsKind::Guard;          break;   // Party Guard (Bear)
    default: break;
    }
    if (es_targets_object(d.esaction)) {                    // sub_524F60 -> the selected object
        es_apply_to_monster(d, level, act);
        return;
    }
    // The classes whose init does the whole work (on the caster, ESClass -1: made at once).
    if (d.esaction == 1111) { esa_weapon_throw(d, level, act); return; }
    if (d.esaction == 1116) { esa_gellder_hit(d, level, act);  return; }
    if (d.esaction == 1165) { esa_multi_gellder_hit(d, level, act); return; }
    if (d.esaction == 1150) { esa_card_throw(d, level, act); return; }
    if (d.esaction == 1145) { esa_sharp_brandish(d, level, act); return; }
    if (kind == EsKind::None) {
        LOG_DEBUG("GameClientSession %u: skill %u ESAction %d (%s) not implemented yet", session_id(), d.id,
                  d.esaction, d.esa_table.c_str());
        return;
    }
    // sub_524F20 -> sub_524690(zone, the caster, action, 1).
    const uint16_t flags = es_flags(d);
    if (d.esclass == -1) { es_create(d, level, act, kind, false, flags); return; }
    bool made;
    if (d.type != 2 && d.id != 7046) {
        if (!es_limiter_admit(d.esclass, d.esaction, static_cast<uint16_t>(d.id))) return;
        made = es_create(d, level, act, kind, true, flags);
    } else {
        made = es_create(d, level, act, kind, false, flags);
    }
    if (made) scroll_use_up(d.esclass, d.esaction);         // (a4 = 1)
}

// The object of an ES whose init did the work at once (ESAWeaponThrow / ESAGellderHit): it stays for `ms`
// (its tick only counts, +208 against +204), out of the limiter (ESClass -1), no warning; then its end
// (vtbl+28): S2C 280 [us][id] to the area.
void GameClientSession::es_push_timer(const SkillDef& d, uint8_t lv, uint16_t id, int32_t ms)
{
    ActiveEs e;
    e.id       = id;
    e.esaction = d.esaction;
    e.estate   = d.estate;
    e.esclass  = d.esclass;
    e.skill    = static_cast<uint16_t>(d.id);
    e.level    = lv;
    e.flags    = es_flags(d);
    e.kind     = EsKind::Timer;
    e.limited  = false;
    e.stay     = true;
    e.result   = 1;
    e.end_ms   = ms;
    e.life_ms  = 0;
    e.warned   = true;
    e.added    = tick_ms();
    es_.push_back(e);
}

// ── PlayerESLimiter vtbl+40 (sub_5E1CE0): may this ES join its ESClass list? ─────────────
//  Not a debuff class: an ES of the same ESAction in the list is ended (replaced). A debuff class
//  (4 / 5 / 6): unless the skill's SkillOption has 4096 (they stack), an ES of the same skill is ended
//  (sub_5E3310 — or, when the skill has a ScrollGroup (+148), one of the same group: no skill of this
//  data has one). Then a list that is not full admits it; a full one evicts the entry added first (the
//  smallest +16) — when that ES is on a player and has a skill, S2C 0x01E4 [4][skill][0] tells it
//  (sub_5FD2E0).
bool GameClientSession::es_limiter_admit(int esclass, int esaction, uint16_t skill)
{
    const int list = es_list(esclass);
    if (list < 0 || list >= kEsLists) return false;
    if (es_debuff_class(esclass)) {
        const SkillDef* sd = server_.skill_db().get(skill);   // sub_625660 (a missing one: a null read)
        if (!sd) return false;
        if (!(sd->skill_option & 4096)) {
            for (size_t i = 0; i < es_.size(); ++i) {
                if (es_[i].limited && es_list(es_[i].esclass) == list && es_[i].skill == skill) {
                    es_end(i);                             // sub_5E3310 -> sub_5E3010(v6, 0)
                    return true;
                }
            }
        }
    } else {
        for (size_t i = 0; i < es_.size(); ++i) {
            if (es_[i].limited && es_list(es_[i].esclass) == list && es_[i].esaction == esaction) {
                es_end(i);                                 // sub_5E31E0 -> sub_5E3010(v5, 0)
                return true;
            }
        }
    }
    int used = 0;
    size_t oldest = es_.size();
    for (size_t i = 0; i < es_.size(); ++i) {
        if (!es_[i].limited || es_list(es_[i].esclass) != list) continue;
        ++used;
        if (oldest == es_.size() || es_[i].added < es_[oldest].added) oldest = i;
    }
    if (used < kPlayerEsCapacity[list]) return true;       // sub_5E3160: a free entry
    if (oldest == es_.size()) return false;                // a list of capacity 0
    if (es_[oldest].skill) send_system_msg(4, es_[oldest].skill, 0);
    es_end(oldest);
    return true;
}

// ── zone sub_7FD4E0: the ES object — InitESAction (sub_50F910) + the class's vtbl+116 ────────
//  false: no object (its init failed).
bool GameClientSession::es_create(const SkillDef& d, uint8_t level, const EsaRow* act, EsKind kind, bool limited,
                                  uint16_t flags, int32_t init_life, uint16_t caster)
{
    bool no_opener = false;
    ActiveEs e;
    e.life_ms  = init_life;                                // InitESAction: +188 = the action's +48
    e.id       = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
    e.esaction = d.esaction;
    e.estate   = d.estate;
    e.esclass  = d.esclass;
    e.skill    = static_cast<uint16_t>(d.id);
    e.level    = level;
    e.flags    = flags;                                    // the action's +16 (es_flags / 2)
    e.kind     = kind;
    e.limited  = limited;
    e.added    = tick_ms();
    e.caster   = caster ? caster : static_cast<uint16_t>(object_id_);
    // vtbl+112 (sub_50F890): the apply roll on the level row's ApplyRatio (+0) and the target's
    // resist of the skill (sub_890AE0: 0 unless the ESAUI picked an object). (Rows whose loader leaves
    // +0 at the constructor's 0.0 — 1387 / 1388 keep ApplyRatio at +8, 1397..1399 / 1423..1425 do not
    // read it — are always applied, no roll.)
    float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    if (kind == EsKind::Field && act) apply_ratio = static_cast<float>(act->get("ApplyRT", 0.0));   // (its +0 is ApplyRT)
    if (!esa_row_has_apply_ratio(d.esaction)) apply_ratio = 0.0f;
    e.result = apply_roll(apply_ratio, 0);

    // vtbl+116, the class's init.
    std::vector<uint8_t> tail;
    MonsterEntity* sc_mob = nullptr;                       // ESASharpClaw's first hit: rolled in its init, its damage
    SkillDamage    sc_hit;                                 //   event in the opener's block
    int32_t        sc_hp_before = 0;
    bool           sc_killed = false;
    auto row_i = [&](const char* col) { return act ? static_cast<int32_t>(act->get(col, 0.0)) : 0; };
    auto row_f = [&](const char* col) { return act ? static_cast<float>(act->get(col, 0.0)) : 0.0f; };
    const int32_t seconds = static_cast<int16_t>(row_i("Seconds"));   // the row's +8 (u16, read as i16)
    switch (kind) {
    case EsKind::StatRatio:
    case EsKind::StatValue:
    case EsKind::PriMarksman:
    case EsKind::CustomizingGun:
        // +212 = the row's +4, +216 = Seconds, +8 = 0, the lifetime 1000 * Seconds (sub_5275B0),
        // the tail [u8 +180] — the apply result. (The scrolls 1301..1305: +212 = Value (i16), +214 =
        // Seconds; tail 1301 none, 1302..1305 [u8 1].)
        if (kind == EsKind::StatRatio || kind == EsKind::CustomizingGun) e.ratio = row_f("Ratio");
        else if (kind == EsKind::StatValue && d.esaction == 1101) e.value = row_i("DX");
        else if (kind == EsKind::StatValue) e.value = static_cast<int16_t>(row_i("Value"));
        else e.value = row_i("AP");
        e.end_ms  = 1000 * seconds;
        e.life_ms = 1000 * seconds;
        if (d.esaction == 1118) {                          // ESAStaticShield: [u16 Seconds] first (sub_554D80)
            tail.push_back(static_cast<uint8_t>(seconds & 0xFF));
            tail.push_back(static_cast<uint8_t>((seconds >> 8) & 0xFF));
        }
        // (ESAAdrenaline sub_572CA0 also writes [u8 +180], but the client's ESAAdrenaline 0x4F56E0 reads none:
        // the client is followed)
        if (d.esaction >= 1302 && d.esaction <= 1305) tail.push_back(1);
        else if (d.esaction != 1301 && d.esaction != 1274) tail.push_back(static_cast<uint8_t>(e.result));
        switch (d.esaction) {
        case 1079: e.stat = 1;  break;   // ESAAcUp       AC (sub_54C3F0)
        case 1092: e.stat = 7;  break;   // ESADaUp       DA (sub_54C980)
        case 1095: e.stat = 11; break;   // ESAHVUp       HV (sub_54BDE0)
        case 1104: e.stat = 4;  break;   // ESAMagicWind  MA (sub_550660)
        case 1112: e.stat = 8;  break;   // ESALuckUp     LK (sub_553520)
        case 1118: e.stat = 10; break;   // ESAStaticShield DP (sub_555030)
        case 1122: e.stat = 5;  break;   // ESAMagicWall    MD (sub_558C80)
        case 1101: e.stat = 2;  break;   // ESAAgilityUp  DX (sub_54F7E0)
        case 1115: e.stat = 1;  break;   // ESAPriMarksManShip AC (sub_553B20)
        case 1301: e.stat = 0;  break;   // ESAApUpScroll AP (sub_57B7A0)
        case 1302: e.stat = 4;  break;   // ESAMaUpScroll MA (sub_57BDA0)
        case 1303: e.stat = 8;  break;   // ESALkUpScroll LK (sub_57C3A0)
        case 1304: e.stat = 11; break;   // ESAHvUpScroll HV (sub_57C9B0)
        case 1305: e.stat = 7;  break;   // ESADaUpScroll DA (sub_57CFC0)
        case 1274: e.stat = 0;  break;   // ESAAdrenaline AP (sub_573130)
        default:   e.stat = -1; break;
        }
        break;
    case EsKind::BearStat: {
        // The Bear's buffs (a reconstruction, skill_bear.cpp): the KR percent-buff rule — stat += (base + items) *
        // Ratio, 1000 * Seconds ms, the same skill replaces itself (the limiter), different skills add — plus the rows'
        // DPRevision as a flat DP (INFERRED from the column name; WEB-KO's Iron Defense master "+70" is its 70).
        // Concentration's "감각력" is taken as DA: Paula's own English data calls it "Detect Ability" like Aura of
        // Anger's 감지력, and the line pairs its buffs (Strength training / Strong Arms AP, Shield of will / Invisible
        // Defence DP, Concentration / Aura of Anger DA). Last Defence (1542) only at HP <= HPRatio * MaxHP — else
        // S2C 239 [0] and no object (its MP is spent, like the KR actions' own checks). Tail [u32 ms] (VERIFIED: the
        // clients' ESAMuscleUp / MindUp / StrongDefense / AngerEnergy / StrongForearm / GoodDefence read a u32 — the
        // icon's time; 1527 WillShield has no client class, its 224 is skipped).
        if (d.esaction == 1542 &&
            static_cast<double>(hp_) > static_cast<double>(hp_max_) * static_cast<double>(row_f("HPRatio"))) {
            skill_cancel_reason(0);
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        e.ratio   = row_f("Ratio");
        e.value   = row_i("DPRevision");
        e.end_ms  = 1000 * seconds;
        e.life_ms = 1000 * seconds;
        if (d.esaction == 1544) {
            // Best Shield (4905, ESClass 3; DpRatio / PDpRatio in %): DP + the equipment DP * DpRatio / 100 on us, *
            // PDpRatio / 100 on the party (bear_party_spread). The report's base is the best member's equipment DP;
            // here it is the caster's (a member's stats are on its own strand) — and "not with other DP buffs" is not
            // checked. No client class reads 1544 (the client's ESABestShield is at 1545): no tail is read.
            int32_t eq[12];
            equip_bonus(eq);
            e.ratio = 0.0f;
            e.value = (caster == 0) ? static_cast<int32_t>(static_cast<double>(eq[10]) *
                                                           static_cast<double>(row_f("DpRatio")) / 100.0)
                                    : 0;                   // (a member's: set by bear_party_es)
        }
        switch (d.esaction) {
        case 1524: case 1541: e.stat = 0;  break;          // MuscleUp / StrongForearm: AP
        case 1525: case 1540: e.stat = 7;  break;          // MindUp / AngerEnergy: DA
        default:              e.stat = 10; break;          // WillShield / StrongDefense / GoodDefence / BestShield: DP
        }
        for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(e.end_ms) >> (8 * k)));
        break;
    }
    case EsKind::HammerWheel: {
        // Hammer Spin (4907, skill_bear.cpp — a reconstruction): the row — Rampage Wheel's (4909, a passive "booster")
        // at its level when learned (hammer_wheel_row) — Ratio, DaRatio, Seconds, TicTime, KillNum, Radius, RunSpeed,
        // ApplyRatio; 1000 * Seconds ms; every TicTime ms a hit on up to KillNum around us within Radius; while it
        // turns our current speed is RunSpeed. Tail [u32 ms] (VERIFIED: the client's ESAHammerWheel sub_5288C0 reads its
        // time, turns the avatar and counts it down itself).
        const EsaRow* r = hammer_wheel_row(d, level, act);
        auto ri = [&](const char* c) { return r ? static_cast<int32_t>(r->get(c, 0.0)) : 0; };
        auto rf = [&](const char* c) { return r ? static_cast<float>(r->get(c, 0.0)) : 0.0f; };
        e.end_ms     = 1000 * static_cast<int16_t>(ri("Seconds"));
        e.life_ms    = e.end_ms;
        e.ratio      = rf("Ratio");
        e.base_ratio = rf("DaRatio");
        e.hp_ratio   = rf("ApplyRatio");
        e.radius     = static_cast<int16_t>(ri("Radius"));
        e.count      = static_cast<int16_t>(ri("KillNum"));
        e.value      = ri("TicTime");
        e.pulse_ms   = 0;
        spin_speed_  = rf("RunSpeed");
        for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(e.end_ms) >> (8 * k)));
        break;
    }
    case EsKind::FlashCut: {
        // ESAFlashAttacking / ESAPowerFlashAttacking (1275 / 1367, the shared init sub_573320; rows sub_51BA50:
        // Ratio +4, Comment +8): +256 = the payload's target, +252 = the row, +200 = us, the target (zone vtbl+16
        // kind 3) — gone -> no object, no packet; one we may not attack (sub_5E6D70: a player without PvP, Type
        // 8 / 12) or our condition 15 -> S2C 239 [u16 0] (sub_5ED7D0) and no object. Else our attack target holder
        // (+232) is let go (sub_41EB30), +260 = our current speed, +248 = the target's spot; the block {S2C 224
        // [us][target][ES id]... + [f32 +260][pos +248]} (VERIFIED: the client's ESAFlashAttacking 0x4F62C0 reads
        // a float and a position); the use TM whatever the roll; +264 = 1 (the run starts on the first tick).
        // Its tick never calls the base one: no lifetime, no 850, the roll unused.
        MonsterEntity* t = su_.target ? server_.entity_mgr().get_monster(su_.target) : nullptr;
        if (!t || t->zone != zone_id_) {
            const GameClientSession* ps = su_.target ? server_.player_session(su_.target) : nullptr;
            if (ps && ps->zone_id_ == zone_id_) skill_cancel_reason(0);   // a player: no PvP here
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        const MobDef* md = server_.mob_db().get(static_cast<int>(t->mob_id));
        if ((md && (md->type == 8 || md->type == 12)) || pcond_test(15)) {
            skill_cancel_reason(0);
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        su_.resume_target = 0;                             // sub_41EB30: the attack target holder let go
        const auto now = std::chrono::steady_clock::now();
        int32_t tx, ty;
        t->motion.pos(now, tx, ty);
        e.target  = static_cast<uint16_t>(t->object_id);
        e.power   = (d.esaction == 1367);
        e.first   = true;
        e.ratio   = cur_speed_;
        e.value   = static_cast<int32_t>((static_cast<uint32_t>(static_cast<uint16_t>(ty)) << 16) |
                                         static_cast<uint16_t>(tx));
        e.end_ms  = INT32_MAX;
        e.life_ms = 0;
        e.stay    = true;
        e.warned  = true;
        {
            uint32_t bits;
            std::memcpy(&bits, &e.ratio, sizeof(bits));
            for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(bits >> (8 * k)));
        }
        for (int32_t v : { tx, ty }) {
            tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) & 0xFF));
            tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8));
        }
        break;
    }
    case EsKind::ThunderBolt: {
        // ESAThunderBolt (1157, Electro Attack, sub_55E850; row sub_51B480: ApplyRatio +0, AP +4, Radius +8 i16,
        // KillNum +10 i16): the payload's target (zone vtbl+16 kind 3) — gone, one we may not attack (sub_5E6D70)
        // or may not hit as another's (vtbl+164) -> no object, no packet. The targets (sub_55EB60): the target
        // first, then the zone's monsters in list order — not dead, our layer, not Type 8 / 12, a shadow one only
        // with the Third Eye — within Radius of the TARGET; such a one once min(KillNum, 10) are taken stops the
        // walk; one other than the target with no owner / ours (our party's) is taken (players in PvP, sub_55EE80).
        // The block: S2C 224 sub_5EDE00 [us][TARGET][ES id]... + [u16 n][u16 id]*n (VERIFIED: the client's
        // ESAThunderBolt 0x4D5270 reads them), then each (sub_55F030): the magic damage sub_5E6270(AP + 3 * Elec
        // (element slot 4, +844), ApplyRatio, MA offset 25, element 16 = Elec), its EXP share, 30002 (a damaging
        // one is marked, +196), then 30003 kind 1; after the block the skill's TM per hit on the target. Its tick
        // (sub_55F210): past 1000 ms once, our Electroshock (es_tick), past 4000 ms the end; no base tick.
        MonsterEntity* t = su_.target ? server_.entity_mgr().get_monster(su_.target) : nullptr;
        const MobDef* md = t ? server_.mob_db().get(static_cast<int>(t->mob_id)) : nullptr;
        if (!t || t->zone != zone_id_ || (md && (md->type == 8 || md->type == 12)) || !may_attack_owned(*t)) {
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        int32_t tx, ty;
        t->motion.pos(now, tx, ty);
        const int16_t radius   = static_cast<int16_t>(row_i("Radius"));
        const int16_t kill_num = static_cast<int16_t>(row_i("KillNum"));
        e.target = static_cast<uint16_t>(t->object_id);
        e.ids.push_back(e.target);
        for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
            MonsterEntity* m = server_.entity_mgr().get_monster(oid);
            if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
            int32_t mx, my;
            m->motion.pos(now, mx, my);
            if (!(static_cast<double>(dist_f(mx, my, tx, ty)) <= static_cast<double>(radius))) continue;
            if (static_cast<int>(e.ids.size()) >= kill_num || e.ids.size() >= 10) break;
            if (oid == e.target) continue;
            if (owned_by_other(*m)) continue;
            e.ids.push_back(static_cast<uint16_t>(oid));
        }
        e.hit.assign(e.ids.size(), 0);
        e.end_ms     = 4000;
        e.life_ms    = 0;
        e.stay       = true;
        e.warned     = true;
        e.shock_done = false;
        const uint16_t n = static_cast<uint16_t>(e.ids.size());
        tail.push_back(static_cast<uint8_t>(n & 0xFF));
        tail.push_back(static_cast<uint8_t>(n >> 8));
        for (uint16_t v : e.ids) { tail.push_back(static_cast<uint8_t>(v & 0xFF)); tail.push_back(static_cast<uint8_t>(v >> 8)); }
        break;
    }
    case EsKind::WindSlash: {
        // ESAWindSlash (1161, Wind Blade, sub_55D080; row sub_51B2A0: ApplyRatio +0, AP +4, Radius +8 i16, KillNum
        // +10 i16): +196 = ApplyRatio, +208 = Radius, +204 = KillNum, the ids (+210) none. The tail [u8 +180] then
        // (sub_55D3A0, only when applied) [u16 n][u16 id]*n — the zone's monsters in list order, not dead, our layer,
        // not Type 8 / 12, a shadow one only with the Third Eye, within Radius of US; such a one once min(KillNum,
        // 10) are taken stops the walk; one without an owner, ours (our party's) or of Type 3 / 4 / 7 / 10
        // (sub_7184B0) is taken (PvP players sub_55D700) — VERIFIED: the client's ESAWindSlash 0x4D2E60 reads [u8]
        // [u16 n]{u16}; the opener is sub_5EE050's [us][us][ES id]... In the same block (sub_55D8C0, applied only)
        // each: the magic damage sub_5E6270(AP + 3 * Wind (element slot 2, +840), ApplyRatio, MA offset 49,
        // element 4 = Wind), its EXP share, 30002, then 30003 kind 1. The use TM whatever the roll. Its tick
        // (sub_55D320): 1500 ms, then the end; not applied: the base tick ends it at once.
        const auto now = std::chrono::steady_clock::now();
        const int16_t radius   = static_cast<int16_t>(row_i("Radius"));
        const int16_t kill_num = static_cast<int16_t>(row_i("KillNum"));
        if (e.result == 1) {
            int32_t cx, cy;
            cur_pos(now, cx, cy);
            for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
                MonsterEntity* m = server_.entity_mgr().get_monster(oid);
                if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
                int32_t mx, my;
                m->motion.pos(now, mx, my);
                if (!(static_cast<double>(dist_f(mx, my, cx, cy)) <= static_cast<double>(radius))) continue;
                if (static_cast<int>(e.ids.size()) >= kill_num || e.ids.size() >= 10) break;
                bool ok = !owned_by_other(*m);
                if (!ok)
                    if (const MobDef* md = server_.mob_db().get(static_cast<int>(m->mob_id)))
                        ok = md->type == 3 || md->type == 4 || md->type == 7 || md->type == 10;
                if (ok) e.ids.push_back(static_cast<uint16_t>(oid));
            }
        }
        e.hit.assign(e.ids.size(), 0);
        e.end_ms  = 1501;                                  // past 1500
        e.life_ms = 0;
        tail.push_back(static_cast<uint8_t>(e.result));
        const uint16_t n = static_cast<uint16_t>(e.ids.size());
        tail.push_back(static_cast<uint8_t>(n & 0xFF));
        tail.push_back(static_cast<uint8_t>(n >> 8));
        for (uint16_t v : e.ids) { tail.push_back(static_cast<uint8_t>(v & 0xFF)); tail.push_back(static_cast<uint8_t>(v >> 8)); }
        break;
    }
    case EsKind::WhirlWind: {
        // ESAWhirlWind (1186, Whirlwind, sub_565EB0; row sub_51B890: ApplyRatio +0, AP +4, Seconds +8, Radius +12 i16,
        // KillNum +14 i16): the row kept, +204 = 1000 * Seconds, +200 = Radius, +196 = KillNum, the timers 0, our spot
        // (+216, only shown to others); the opener + [u32 Radius] (VERIFIED: the client's ESAWhirlWind 0x4DADB0 reads
        // a u32); the use TM whatever the roll. Its tick (sub_566140): past +204, or we move (+92 not standing), the
        // end; every 2000 ms the pulse (whirlwind_pulse); then the base tick (not applied: the end).
        const int32_t radius = static_cast<int16_t>(row_i("Radius"));
        e.end_ms   = 1000 * row_i("Seconds");
        e.life_ms  = e.end_ms;
        e.radius   = radius;
        e.count    = static_cast<int16_t>(row_i("KillNum"));
        e.pulse_ms = 0;
        for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(radius) >> (8 * k)));
        break;
    }
    case EsKind::ElemShield:
        // ESAWaterShield / ESAElectricShield / ESAFireShield / ESAEarthShield / ESAWindShield (1247 / 1250 / 1251 /
        // 1252 / 1253, sub_56B000 ...; row sub_51C2A0: Value +4 i16, Seconds +6 i16, ApplyRatio +0): +196 = Value, +204
        // = 1000 * Seconds, the lifetime +204, the tail [u8 +180] (VERIFIED: the client's ESAWaterShield 0x4E92D0 and
        // its siblings read it); applied -> the target's +924 slot WaterR 12 / ElecR 15 / FireR 11 / EarthR 14 /
        // WindR 13 += Value (sub_56B190) with S2C 826, and the use TM. Its end: the Spiritualist's blessing that
        // needs it (1337 / 1338 / 1339 / 1340 / 1341) ends (the limiter's vtbl+60), then the Value comes off.
        e.value   = static_cast<int16_t>(row_i("Value"));
        e.end_ms  = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.life_ms = e.end_ms;
        e.count   = (d.esaction == 1247) ? 12 : (d.esaction == 1250) ? 15 : (d.esaction == 1251) ? 11 :
                    (d.esaction == 1252) ? 14 : 13;
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::Field: {
        // ESAFireField / ESABreakdownGround (1353 / 1358, sub_53DBF0 / sub_53EE90; rows sub_51CB30 / sub_51CC80: ApplyRT
        // +0, M_AP +4, Radius +8, Seconds +10, KillNum +12, (SpeedDown +16)): the row kept, the spot = the payload's
        // point (sub_53E780 -> sub_7149C0), +240 = 1000 * Seconds, +248 = Radius, +250 = min(KillNum, 7), its set of
        // ones in it empty, the lifetime +240; the opener + [u8 +180][u16 Radius][pos] — and [u32 its time]: the
        // client's ESAFireField 0x509E00 / ESABreakdownGround 0x50B420 read one more u32 than the recovered server
        // writes, so the field's 1000 * Seconds is sent —; the use TM whatever the roll. Its tick (sub_53DEB0): past
        // +240 or without us the end; every 1000 ms the pulse (field_pulse_es); the base tick (not applied: the end).
        // Its end (sub_53E6A0): the Do (1354 / 1359) on each one of its set ends.
        int16_t px = 0, py = 0;
        if (su_.payload.size() >= 4) {
            px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
            py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
        }
        e.fx       = px;
        e.fy       = py;
        e.radius   = static_cast<int16_t>(row_i("Radius"));
        e.count    = std::min<int>(static_cast<int16_t>(row_i("KillNum")), 7);
        e.end_ms   = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.life_ms  = e.end_ms;
        e.pulse_ms = 0;
        tail.push_back(static_cast<uint8_t>(e.result));
        for (uint16_t v : { static_cast<uint16_t>(e.radius), static_cast<uint16_t>(px), static_cast<uint16_t>(py) }) {
            tail.push_back(static_cast<uint8_t>(v & 0xFF));
            tail.push_back(static_cast<uint8_t>(v >> 8));
        }
        for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(e.end_ms) >> (8 * k)));
        break;
    }
    case EsKind::GaleBreak: {
        // ESAGaleBreak (1357, Tornado Blast, sub_586980; row sub_51CDD0: MagicAP +4, Radius +6, Seconds +8, KillNum +10,
        // Comment +12, ApplyRatio +16 — nothing at +0: always applied): the payload's point (+196), the delay +204 =
        // 1000 * Seconds, +200 = Radius, +208 = 3 * our Wind + MagicAP, +210 = KillNum, +212 = ApplyRatio; the opener +
        // [u8 +180][pos][u16 Radius][u32 +204] (VERIFIED: the client's ESAGaleBreak 0x50F1C0 reads them); applied: the
        // use TM. Its tick (sub_586D50): +204 counted down; past 0 once, the hit (gale_break_hit); 3000 ms after it, the
        // end; the base tick.
        int16_t px = 0, py = 0;
        if (su_.payload.size() >= 4) {
            px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
            py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
        }
        e.fx       = px;
        e.fy       = py;
        e.radius   = static_cast<int16_t>(row_i("Radius"));
        e.value    = static_cast<int16_t>(3 * elem_slot(2) + static_cast<int16_t>(row_i("MagicAP")));
        e.count    = static_cast<int16_t>(row_i("KillNum"));
        e.ratio    = row_f("ApplyRatio");
        e.end_ms   = 1000 * static_cast<int16_t>(row_i("Seconds"));   // +204: the delay
        e.life_ms  = 0;
        e.first    = false;                                // +216: the hit done
        e.pulse_ms = 0;                                    // +218: the time since
        tail.push_back(static_cast<uint8_t>(e.result));
        for (uint16_t v : { static_cast<uint16_t>(px), static_cast<uint16_t>(py), static_cast<uint16_t>(e.radius) }) {
            tail.push_back(static_cast<uint8_t>(v & 0xFF));
            tail.push_back(static_cast<uint8_t>(v >> 8));
        }
        for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(e.end_ms) >> (8 * k)));
        break;
    }
    case EsKind::Bind:
        // ESABind (1154, Hinder Snare, init sub_534170; row sub_51B080: ApplyRatio +0, Ratio +4, Seconds +8 i16, Radius
        // +10, Comment +12): +196 = 1000 * Seconds, +200 = 0, its list empty; no lifetime (+188 = 0: the 10 s warning at
        // its first base tick); the opener + [u8 +180] (VERIFIED: the client's ESABind 0x4CFE70 reads it); applied: we
        // stop, the condition 10, the use TM, ESABindDo on those around (bind_children), +308 = 1. Its handler of a
        // damage event on us (sub_534B50): +308 -> the end. Its tick (sub_5349F0): a dead one's ESABindDo ends and
        // leaves the list; past +196, or we move -> the end; the base tick (twice while we stand). Its release
        // (vtbl+36 sub_534BF0, +308): its ESABindDo end, the condition 10 off.
        e.end_ms  = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.radius  = static_cast<uint16_t>(row_i("Radius"));
        e.first   = false;
        e.life_ms = init_life;
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::DarkBarrier:
        // ESADarkBarrier (1254, init sub_56D380; row sub_51C4A0: ApplyRatio +0, Value +4 i16, ReflexRatio +8, Seconds
        // +12 i16, Comment +14): +236 = Value, +244 = 1000 * Seconds, the lifetime, +232 = ReflexRatio; the opener +
        // [u8 +180] (VERIFIED: the client's ESADarkBarrier 0x4EAEF0 reads it); applied: DarkR (+924 slot 17) += Value
        // with S2C 826 (sub_56D540(1)) and the use TM. Its handler of a damage event on us (sub_56D7E0, mob_swing):
        // part of the blow comes back to the attacker. Its tick (sub_56D770): past +244 the end; its end: DarkR back.
        e.value   = static_cast<int16_t>(row_i("Value"));
        e.end_ms  = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.life_ms = e.end_ms;
        e.ratio   = row_f("ReflexRatio");
        e.first   = false;                                  // +224: a blow to send back pending
        e.interval_acc = 0;                                 // +228
        e.other   = 0;                                      // +248
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::LightShield:
        // ESALightShield (1255, init sub_56CBC0; row sub_51C410: ApplyRatio +0, Value +4 i16, DpPRatio +8, MdPRatio +12,
        // Seconds +16 i16, Comment +18): +212 = Value, +228 = 1000 * Seconds, the lifetime, +220 = DpPRatio, +216 =
        // MdPRatio; the opener + [u8 +180] (VERIFIED: the client's ESALightShield 0x4EAA40 reads it); then whatever the
        // roll: its StatusValueAdder (sub_7172E0: MD += 16 * (our MA - 49) * MdPRatio, DP += .. * DpPRatio, each when
        // above 0 — sub_56CEC0), LightR (+924 slot 16) += Value with S2C 826 (sub_56CD90(1)), the recompute + S2C 283
        // [2][5][MD buff][10][DP buff] (sub_56CE40) and the use TM. Its tick (sub_56D190): past +228 the end; its end
        // (sub_56CF80): the adder out, LightR back, the recompute + S2C 283.
        e.value      = static_cast<int16_t>(row_i("Value"));
        e.end_ms     = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.life_ms    = e.end_ms;
        e.ratio      = row_f("DpPRatio");
        e.base_ratio = row_f("MdPRatio");
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::HolyShield:
        // ESAShield (1333, Shield of Heaven, init sub_526E70; row sub_51C9F0: ApplyRatio +0, Ratio +4, Seconds +8 i16,
        // RecoverySec +10 i16, Comment +12): +224 = 1000 * Seconds, the lifetime, +236 = 1000 * RecoverySec; applied
        // whatever the roll (sub_5275E0(1)); the shield +232 = (int)(our MA * 15.0 * Ratio), +228 = it; the opener +
        // [u32 +228][u32 +232] (VERIFIED: the client's ESAShield 0x504AC0 reads them). No TM. Its handler of a damage
        // event on us (sub_5271D0, mob_swing): it takes the blow (S2C 921). Its tick (sub_527050): past +224 or broken
        // -> the end; every RecoverySec it refills by (int)(+232 * 0.1) (at most +232) and S2C 921 [u16 its id][u32
        // shield][u32 max] (sub_605AA0; VERIFIED: the client's ESAShield sub_504990 reads them for 921).
        e.first        = false;                             // +220: broken
        e.end_ms       = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.interval_ms  = 1000 * static_cast<int16_t>(row_i("RecoverySec"));
        e.interval_acc = 0;
        e.result       = 1;
        e.count        = static_cast<int32_t>(static_cast<double>(stat_value(4)) * 15.0 * static_cast<double>(row_f("Ratio")));
        e.value        = e.count;
        e.life_ms      = e.end_ms;
        for (int32_t v : { e.value, e.count })
            for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
        break;
    case EsKind::BloodyOath:
        // ESABloodyOath (1360, Blood Testament, init sub_586280; row sub_51CD70: MARatio +4, HPRatio +8, Seconds +12 i16,
        // Comment +14 — its +0 stays 0.0: always applied): +196 = 1000 * Seconds (counted down), the lifetime, +200 =
        // HPRatio; whatever the roll our +1616 = 1, +1620 = HPRatio, +1624 = MARatio (sub_586420 / 440 / 460); the opener
        // + [u8 +180] (VERIFIED: the client's ESABloodyOath 0x50ED70 reads it); applied: the use TM. Its handler of the
        // 30097 (bloody_on_dealt); its tick (sub_586560): +196 below 0 -> the end; its end: applied -> +1616 = 0.
        e.end_ms  = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.value   = e.end_ms;
        e.life_ms = e.end_ms;
        e.ratio   = row_f("HPRatio");
        bloody_on_ = true;
        bloody_hp_ = e.ratio;
        bloody_ma_ = row_f("MARatio");
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::SaintPromise: {
        // ESASaintPromise (1361, Oath of Light, init sub_585D10; row sub_51CD20: HPRatio +4, Seconds +8 i16, Comment +10
        // — its +0 stays 0.0: always applied): +204 = 3000, +196 = (int)(our MaxHP * HPRatio), +200 = 1000 * Seconds
        // (counted down), the lifetime; the opener + [u8 +180][u32 +196] (VERIFIED: the client's ESASaintPromise
        // 0x50EA70 reads them); applied: the use TM. Its tick (sub_585FC0): +204 below 0 -> HP + +196 (from below MaxHP,
        // at most MaxHP) and S2C 0x24 [u32 HP] (sub_586050), +204 = 3000; +200 below 0 -> the end.
        e.interval_acc = 3000;
        e.value        = static_cast<int32_t>(static_cast<double>(hp_max_) * static_cast<double>(row_f("HPRatio")));
        e.end_ms       = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.count        = e.end_ms;                          // +200, counted down
        e.life_ms      = e.end_ms;
        tail.push_back(static_cast<uint8_t>(e.result));
        for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(e.value) >> (8 * k)));
        break;
    }
    case EsKind::SkyRay: {
        // ESASkyRay (1285, Searing Light, init sub_570570; row sub_51BFC0: ApplyRatio +0, AP +4, Radius +8 i16): +196 =
        // 0, us (+200); the payload's target (zone vtbl+16 kind 3) — none: no object; applied: sub_53E360 around the
        // TARGET, at most 5, Radius — the target not among them takes the first one's place; the block: the opener
        // sub_5EE050 [us][us][ES id]... + [u8 +180][u16 n][u16 id]*n (VERIFIED: the client's ESASkyRay 0x4F3290 reads
        // them), then applied each (sub_570870): the magic damage sub_5E6270(AP, ApplyRatio, MA offset 25, element 32 =
        // Light), its EXP share, 30002, 30003 kind 1; applied: 3000 ms of protection for us (sub_73CF80(3000, 0)) and
        // the use TM. Its tick (sub_570980): without us or past 3000 ms the end; the base tick.
        const auto now = std::chrono::steady_clock::now();
        MonsterEntity* t = su_.target ? server_.entity_mgr().get_monster(su_.target) : nullptr;
        if (!t || t->zone != zone_id_) {
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        e.target     = su_.target;
        e.end_ms     = 3000;
        e.countdown  = true;                                // (past 3000 ms, not at it)
        e.life_ms    = init_life;
        std::vector<MonsterEntity*> arr;
        if (e.result == 1) {
            int32_t tx, ty;
            t->motion.pos(now, tx, ty);
            arr = area_monsters(tx, ty, static_cast<int16_t>(row_i("Radius")), 5, now);
            if (!arr.empty() && std::find(arr.begin(), arr.end(), t) == arr.end()) arr[0] = t;
        }
        tail.push_back(static_cast<uint8_t>(e.result));
        tail.push_back(static_cast<uint8_t>(arr.size() & 0xFF));
        tail.push_back(static_cast<uint8_t>(arr.size() >> 8));
        for (MonsterEntity* m : arr) {
            e.ids.push_back(static_cast<uint16_t>(m->object_id));
            tail.push_back(static_cast<uint8_t>(m->object_id & 0xFF));
            tail.push_back(static_cast<uint8_t>((m->object_id >> 8) & 0xFF));
        }
        e.hit.assign(e.ids.size(), 0);                      // (the opener's hit loop marks the ones it damaged)
        break;
    }
    case EsKind::SaintOath: {
        // ESASaintOath (1379, Sacred Oath, init sub_58B840; row sub_51D010: HPRatio +4, Seconds +8, Comment +10, Radius +12
        // — its +0 stays 0.0: always applied): +196 = 1000 (counted down); no opener of its own; our share group's
        // players on our layer within Radius, at most 6 (sub_58B9F0: us among them; no group: none); the action
        // {ESAction 1386, EState 0, ESClass 0, flags 18, skill 2504, this row} through sub_524690(.., 1) on us, then on
        // each of them — its ref set to that one: each its own caster; us again (ours replaced, retail) —
        // ESASaintPromise (es_create SaintPromise; its own opener and TM; a member's on its strand) —; applied: the use
        // TM. Its tick (sub_58BB00): +196 below 0 -> the end.
        e.value   = 1000;
        e.life_ms = init_life;
        no_opener = true;
        {
            const uint16_t sk = static_cast<uint16_t>(d.id);
            party_action_from(0, sk, level, 1386, zone_id_);
            for (GameClientSession* s : party_in_radius(static_cast<int16_t>(row_i("Radius")), 6)) {
                if (s == this) party_action_from(0, sk, level, 1386, zone_id_);
                else s->party_action_pulse(0, sk, level, 1386, zone_id_);
            }
        }
        break;
    }
    case EsKind::SettleDown: {
        // ESASettleDownDo (1487, init sub_5B9D20; Refreshing Dew's row: Ratio +4 — the loader keeps 1 / Ratio —, Seconds
        // +8 int): only on a player; +196 = 1000 * Seconds, +200 = 1 / Ratio, applied whatever the roll (sub_5275E0(1)),
        // the lifetime +196; our cool times' speed (+3036 + 68, sub_5BBD30) = +200; the opener + [float +200] (VERIFIED:
        // the client's ESASettleDownDo 0x51F7C0 reads the float — its own cool times, GameWorld +8636). Its tick
        // (sub_5B9EB0): past +196 the end; its end (sub_5B9F20): the speed back to 1.0.
        const float r = row_f("Ratio");
        e.ratio   = (r != 0.0f) ? static_cast<float>(1.0 / static_cast<double>(r)) : r;
        e.end_ms  = 1000 * row_i("Seconds");
        e.result  = 1;
        e.life_ms = e.end_ms;
        cd_ratio_ = e.ratio;
        uint32_t bits = 0;
        std::memcpy(&bits, &e.ratio, 4);
        for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(bits >> (8 * k)));
        break;
    }
    case EsKind::DarkGhost:
        // ESADarkGhost (1342, Dark Commando, init sub_583720; row sub_51CAC0: ApplyRatio +0, M_AP +4 i16, Seconds +6 i16,
        // Comment +8, APRatio +12): +196 = M_AP, +204 = Seconds, +200 = APRatio; the lifetime 1000 * Seconds; the opener +
        // [u8 +180] (VERIFIED: the client's ESADarkGhost 0x5085B0 reads it); applied: the use TM. Its handler of the 30097
        // (dark_ghost_on_dealt); its tick (sub_583960): past 1000 * Seconds the end.
        e.value   = static_cast<int16_t>(row_i("M_AP"));
        e.end_ms  = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.life_ms = e.end_ms;
        e.ratio   = row_f("APRatio");
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::DarkWhisper: {
        // ESADarkWhisper (1380, Ghostly Whisper, init sub_58BD00; row sub_51D070: M_AP +4, Seconds +6, KillNum +8, Radius
        // +10 (i16s), ApplyRatio +12, Comment +16 — its +0 stays 0.0: always applied): +196 = (i16)(1000 * Seconds), +198 =
        // 500, +200 = (u8)KillNum, +201 = our spot, +206 = Radius, +208 = M_AP, +212 = ApplyRatio; no lifetime (the 10 s
        // warning at its first base tick); the opener + [u8 +180][pos][u16 Radius] (VERIFIED: the client's ESADarkWhisper
        // 0x514500 reads them); applied: the use TM. Its tick (sub_58BF90): +196 / +198 counted down; still on its spot
        // with +196 left: +198 at 0 -> the pulse (dark_whisper_pulse), +198 = 2000; the base tick; else the end. The
        // message 87 (an item use) ends it (sub_58C2F0, es_on_item_use).
        const auto now = std::chrono::steady_clock::now();
        int32_t cx, cy;
        cur_pos(now, cx, cy);
        e.value        = static_cast<int16_t>(1000 * row_i("Seconds"));
        e.interval_acc = 500;
        e.count        = static_cast<uint8_t>(row_i("KillNum"));
        e.fx           = static_cast<int16_t>(cx);
        e.fy           = static_cast<int16_t>(cy);
        e.radius       = static_cast<int16_t>(row_i("Radius"));
        e.dinfo[0]     = static_cast<int16_t>(row_i("M_AP"));
        e.ratio        = row_f("ApplyRatio");
        e.life_ms      = init_life;
        tail.push_back(static_cast<uint8_t>(e.result));
        for (uint16_t v : { static_cast<uint16_t>(e.fx), static_cast<uint16_t>(e.fy), static_cast<uint16_t>(e.radius) }) {
            tail.push_back(static_cast<uint8_t>(v & 0xFF));
            tail.push_back(static_cast<uint8_t>(v >> 8));
        }
        break;
    }
    case EsKind::DarkUp:
        // ESADarkUpDo (1489, init sub_5BA080; Dark Assimilation's row: Ratio +4, Seconds +8 int): +196 = 1000 * Seconds;
        // applied whatever the roll (sub_5275E0(1)); the lifetime +196; +200 = (int)(our Dark (+848) * Ratio / 100.0); the
        // opener without a tail (VERIFIED: the client's ESADarkUpDo 0x51F8B0 reads none); then +924 slot 6 += +200 with
        // S2C 826 (sub_5BA1F0(1)). No TM. Its tick (sub_5BA2D0): past +196 the end; its end: the value off.
        e.end_ms  = 1000 * row_i("Seconds");
        e.result  = 1;
        e.life_ms = e.end_ms;
        e.value   = static_cast<int16_t>(static_cast<int>(static_cast<double>(elem_slot(6)) *
                                                          static_cast<double>(row_f("Ratio")) / 100.0));
        break;
    case EsKind::Counter: {
        // ESAWaterCounter / ESAThunderCounter / ESAFireCounter / ESAEarthCounter / ESAWindCounter (1337..1341, the
        // blessings; sub_580DF0 / sub_581440 / sub_581A60 / sub_582080 / sub_5826A0; row sub_51C070: ApplyRatio +0, AP +4,
        // Comment +8): a player without its shield (the limiter's vtbl+80: 1247 / 1250 / 1251 / 1252 / 1253) -> no object;
        // +196 = AP + 3 * our Water / Elec / Fire / Earth / Wind (element slot 1 / 4 / 0 / 3 / 2), +200 = ApplyRatio; the
        // lifetime 10000000 (ESAWaterCounter: the immediate IDA names "mycamp_change_chat_info", 0x989680) / 1000000;
        // the opener + [u8 +180] (VERIFIED: the client's ESAWaterCounter 0x505480 and its siblings read it); applied:
        // its message 30015 in 1000000 ms (its end) and the use TM; not applied: 30015 at once. Its tick is the base
        // tick; the shield's end ends it; its hit back: counter_on_hit (a damage event on us).
        static const int kShield[5] = { 1247, 1250, 1251, 1252, 1253 };
        static const int kSlot[5]   = { 1, 4, 0, 3, 2 };
        const int c = d.esaction - 1337;
        const bool shield = es_find(kShield[c]) != nullptr;  // the limiter's vtbl+80
        if (!shield) {
            LOG_DEBUG("GameClientSession %u: blessing %d without its shield %d — no object", session_id(), d.esaction,
                      kShield[c]);
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        e.value   = row_i("AP") + 3 * elem_slot(kSlot[c]);
        e.ratio   = row_f("ApplyRatio");
        e.end_ms  = (e.result == 1) ? 1000000 : 0;
        e.life_ms = (d.esaction == 1337) ? 10000000 : 1000000;
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    }
    case EsKind::ElemArea: {
        // ESAWaterArea .. ESAWindArea (1369 / 1371 / 1373 / 1375 / 1377, the gardens; init sub_589040, then its class's:
        // +260 = its Do 1370 / 1372 / ...; row sub_51CF80: Radius +4, Count +6, RegiDown +8, Seconds +12, ApplyRatio +16,
        // Comment +20 — its +0 stays the constructor's 0.0: always applied): the spot = the payload's point (sub_53E780
        // -> sub_7149C0), +248 = 1000 * Seconds, the lifetime, +256 = Radius, +258 = min(Count, 5), its set empty; the
        // opener + [u8 +180][u16 Radius][pos] (VERIFIED: the client's ESAWaterArea 0x511FF0 and its siblings read them);
        // our use TM whatever the roll. Its tick (sub_5892B0): without us or past +248 the end; every 1000 ms the pulse
        // (elem_area_pulse); the base tick. Its end (sub_5899F0): the Do on each one of its set ends.
        int16_t px = 0, py = 0;
        if (su_.payload.size() >= 4) {
            px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
            py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
        }
        e.fx       = px;
        e.fy       = py;
        e.radius   = static_cast<int16_t>(row_i("Radius"));
        e.count    = std::min<int>(static_cast<int16_t>(row_i("Count")), 5);
        e.end_ms   = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.life_ms  = e.end_ms;
        e.pulse_ms = 0;
        tail.push_back(static_cast<uint8_t>(e.result));
        for (uint16_t v : { static_cast<uint16_t>(e.radius), static_cast<uint16_t>(px), static_cast<uint16_t>(py) }) {
            tail.push_back(static_cast<uint8_t>(v & 0xFF));
            tail.push_back(static_cast<uint8_t>(v >> 8));
        }
        break;
    }
    case EsKind::ElementUp:
        // ESAElementUpDo (1473, sub_5B7360; Elemental Boost's row: Seconds +4, Ratio +8): +208 = 1000 * Seconds, the
        // five values 0; kept whatever the roll (+184 = 1); the lifetime +208; the values (sub_5B74A0) = (int)(our
        // Fire / Water / Wind / Earth / Elec (+836..+844) * Ratio / 100.0); the opener without a tail (VERIFIED: the
        // client's ESAElementUpDo 0x51D8C0 reads none); then +924 [0..4] += them with S2C 826 (sub_5B75D0). Its tick
        // (sub_5B7850): past +208 the end; the end takes them off.
        e.end_ms  = 1000 * row_i("Seconds");
        e.life_ms = e.end_ms;
        e.stay    = true;
        for (int k = 0; k < 5; ++k)
            e.bonus[k] = static_cast<int16_t>(static_cast<int>(static_cast<double>(elem_slot(k)) *
                                                               static_cast<double>(row_f("Ratio")) / 100.0));
        break;
    case EsKind::EnergySword:
        // ESAEnergySword (1368, sub_588850; row sub_51CF30: Range +4, Seconds +6, Comment +8 — no ApplyRatio:
        // always applied): a player target with an item in its equipment slot 1 (sub_41C7D0(+2120, 1)), else S2C
        // 239 [u16 0] (sub_5ED7D0) and no object. +196 = Range, +204 = 1000 * Seconds, the lifetime +204, the
        // opener without a tail (VERIFIED: the client's ESAEnergySword 0x511A10 reads none), then sub_588A40(1):
        // our ranges +1044 / +1048 / +1052 (approach / Max / Chase) += Range and +0x148 flag 64 (our melee
        // blows then carry DamageInfo flag 0x400, sub_5E5700); the use TM. Its tick (sub_588C80) ends it at
        // +204 or with slot 1 empty; its end takes the ranges and the flag back.
        if (!equip_[1]) {
            skill_cancel_reason(0);
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        e.value    = static_cast<int16_t>(row_i("Range"));
        e.end_ms   = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.life_ms  = e.end_ms;
        e.adder_on = true;
        break;
    case EsKind::SelfCure: {
        // ESASelfCureDo (1483, sub_5B94E0; row sub_51DE10: ApplyRatio +0, Heal +4, Interval +8, Time +12 — ints —,
        // ESAction0..4 +16..): +196 = 1000 * Time, +200 = 1000 * Interval, +208 = Heal, +204 = 0; the target's ESs
        // of ESAction0..4 end (the limiter's vtbl+60); kept whatever the roll (+184 = 1); the lifetime +196; the
        // opener + [u32 +200][u32 +208] (VERIFIED: the client's ESASelfCureDo 0x51F0C0 reads two u32). Its tick
        // (sub_5B96B0): past +196 the end, else every +200 HP += Heal up to MaxHP (sub_718F20 -> S2C 0x0024); a
        // damage event on us ends it (its vtbl+8, sub_5B9780).
        e.end_ms       = 1000 * row_i("Time");
        e.interval_ms  = 1000 * row_i("Interval");
        e.value        = row_i("Heal");
        e.interval_acc = 0;
        e.life_ms      = e.end_ms;
        e.stay         = true;
        for (int k = 0; k < 5; ++k) {
            char col[16];
            std::snprintf(col, sizeof(col), "ESAction%d", k);
            const int esa = row_i(col);
            if (esa && es_find(esa)) es_remove_esaction(esa);
        }
        for (int32_t v : { e.interval_ms, e.value })
            for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
        break;
    }
    case EsKind::FastLoad:
        // sub_5B7A10: +196 = 1000 * the row's Seconds (int, +4), +200 = the Ratio (float, +8),
        // +184 = 1 (kept although not applied), the lifetime +196, the tail [f32 Ratio].
        e.end_ms  = 1000 * row_i("Seconds");
        e.life_ms = e.end_ms;
        e.ratio   = row_f("Ratio");
        e.stay    = true;
        {
            uint32_t bits;
            std::memcpy(&bits, &e.ratio, sizeof(bits));
            for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(bits >> (8 * k)));
        }
        break;
    case EsKind::SharpSense:
        // sub_568B30: +196 = Ratio, +200 = 1000 * Seconds; NOT applied -> the init fails and the
        // object goes away without a packet (sub_7FD4E0); applied -> the lifetime +200, the tail
        // [u8 +180], then the target's +1064 = Ratio (sub_568CF0) and its TM (a player target).
        if (e.result != 1) {
            LOG_DEBUG("GameClientSession %u: ES ESAction %d not applied (result %d) — no object", session_id(),
                      e.esaction, e.result);
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        e.ratio   = row_f("Ratio");
        e.end_ms  = 1000 * seconds;
        e.life_ms = e.end_ms;
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::ThirdEye:
        // sub_58E9C0: +196 = the row's Second (+8, i16; loader sub_51BEF0: Ratio +4, Second +8,
        // Comment +10), the lifetime and the end 1000 * Second, the tail [u8 +180]; then the event 30280
        // to the target — whatever the apply roll (ESAction_ThirdEye's ApplyRatio is 1: always applied).
        e.end_ms  = 1000 * static_cast<int16_t>(row_i("Second"));
        e.life_ms = e.end_ms;
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::Boiling:
        // sub_58EE10: +196 = 1000 * the row's Second (+8, i16), +200 = its StressUp (+4, float), the
        // lifetime +196, the tail [u8 +180]; applied -> the skill's TM and the Crazy Drill boost
        // (sub_76A460 -> CCrazyDrillMgr +64 = StressUp).
        e.end_ms  = 1000 * static_cast<int16_t>(row_i("Second"));
        e.life_ms = e.end_ms;
        e.ratio   = row_f("StressUp");
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::ScrollShell:
        // sub_58D700: +196 = 1000 counted down (no lifetime: +188 stays 0), then the Do on the caster
        // (sub_524690(zone, caster, the 1388 action, 0)) — and, for PartySkillType 2 (the PT_*UpScroll rows), on
        // each of our share group's players on our layer within Radius, at most 6 (sub_58B9F0: us among them —
        // ours made again, retail; no group: none), each its own caster (a member's on its strand). No packet of its own.
        e.end_ms    = 1000;
        e.life_ms   = 0;
        e.countdown = true;
        es_scroll_do(d, level, act);
        if (static_cast<int16_t>(row_i("PartySkillType")) == 2) {
            const uint16_t sk = static_cast<uint16_t>(d.id);
            for (GameClientSession* s : party_in_radius(static_cast<int16_t>(row_i("Radius")), 6)) {
                if (s == this) party_action_from(0, sk, level, 1388, zone_id_);
                else s->party_action_pulse(0, sk, level, 1388, zone_id_);
            }
        }
        break;
    case EsKind::ScrollStatUp: {
        // sub_58DB10 (the action carries the 1387 skill's row).
        static const char* const kRatio[12] = { "ApUpRatio", "AcUpRatio", "DxUpRatio", "MpUpRatio", "MaUpRatio",
                                                "MdUpRatio", "WtUpRatio", "DaUpRatio", "LkUpRatio", "HpUpRatio",
                                                "DpUpRatio", "HvUpRatio" };
        // CalculateType 2: AC, MA, DA, LK and HV take Lv * (int)ratio / 40, DX 0, the others
        // 4 * Lv * (int)ratio / 10 (Lv = the target's, Player +1020).
        static const bool kByForty[12] = { false, true, false, false, true, false, false, true, true, false, false, true };
        const int calc = static_cast<int16_t>(row_i("CalculateType"));
        for (int i = 0; i < 12; ++i) {
            const float r = row_f(kRatio[i]);
            if (!(r > 0.0f)) continue;
            const int ri = static_cast<int>(r);
            if (calc == 1)       e.bonus[i] = ri;
            else if (calc == 2)  e.bonus[i] = (i == 2) ? 0 : (kByForty[i] ? level_ * ri / 40 : 4 * level_ * ri / 10);
        }
        e.end_ms    = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.life_ms   = e.end_ms;
        e.countdown = true;
        tail.push_back(static_cast<uint8_t>(e.result));
        for (const char* col : { "BeginEffect", "ProcessEffect", "BeginSound", "ProcessSound" }) {   // sub_43ADF0
            const std::string& s = act ? act->text(col) : std::string();
            tail.insert(tail.end(), s.begin(), s.end());
            tail.push_back(0);
        }
        break;
    }
    case EsKind::Marker:
        // sub_58FAA0 (1397 / 1398 / 1399): +196 = the row's Seconds (+4, i16), +8 = 0, the lifetime
        // 1000 * Seconds; no tail (VERIFIED: the client's ESAPrevent*Scroll sub_5176F0 reads none). Its
        // tick ends it at 1000 * Seconds (sub_58FC70). Others look it up (the limiter's vtbl+84): 1399 in
        // OnDamaged (mob_swing), 1398 against the monsters' stuns, 1397 against their warps.
        e.end_ms  = 1000 * seconds;
        e.life_ms = e.end_ms;
        break;
    case EsKind::Light: {
        // sub_590120 (1423, loader sub_51D520: Seconds +4, Radius +6): +200 = Seconds, +196 = Radius, the
        // lifetime 1000 * Seconds, the tail [u32 Radius][u32 Seconds] (VERIFIED: the client's ESALightScroll
        // sub_51AB50 reads two u32 — for its own avatar only, the light radius sub_703E00).
        const int32_t radius = static_cast<int16_t>(row_i("Radius"));
        e.end_ms  = 1000 * seconds;
        e.life_ms = e.end_ms;
        e.value   = radius;
        for (int32_t v : { radius, static_cast<int32_t>(seconds) })
            for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
        break;
    }
    case EsKind::WideHealing: {
        // sub_5905C0 (1424, loader sub_51D570: Ratio +4 float, Radius +8): the caster's HP += (int)(MaxHP *
        // Ratio) up to MaxHP (S2C 0x0024 to it, sub_5F4670), then each other living character of the zone
        // in the same layer (sub_893F00) within Radius (sub_42F1B0) the same, collected by id (a set);
        // the tail [u8 result][u32 n]{u16 id} (VERIFIED: the client's ESAWideHealingScroll sub_51AC90
        // reads them, plays "heal" and shows SkillStrings "Notice_WideHealing" to a healed player). Its
        // tick is the base one alone: it never ends by itself (no lifetime: the 10 s warning at once).
        const float ratio = row_f("Ratio");
        const int radius  = static_cast<int16_t>(row_i("Radius"));
        auto heal = [ratio](int32_t& hp, int32_t max) {
            const float h = static_cast<float>(static_cast<double>(max) * ratio);
            int32_t v = hp + static_cast<int32_t>(h);
            if (v > max) v = max;
            hp = v;
        };
        heal(hp_, hp_max_);
        {
            PacketBuffer p(static_cast<Opcode>(0x0024), is_paula());
            p.write<uint32_t>(static_cast<uint32_t>(hp_));
            send(p);
        }
        const auto now = std::chrono::steady_clock::now();
        int32_t cx, cy;
        cur_pos(now, cx, cy);
        std::vector<uint16_t> ids;
        for (uint32_t oid : server_.entity_mgr().players_in_zone(zone_id_)) {
            GameClientSession* s = server_.player_session(oid);
            if (!s || s == this || s->object_id() == 0 || s->dead_ || s->hp_ <= 0) continue;
            int32_t sx, sy;
            s->cur_pos(now, sx, sy);
            if (static_cast<double>(dist_f(cx, cy, sx, sy)) > static_cast<double>(radius)) continue;
            heal(s->hp_, s->hp_max_);
            PacketBuffer p(static_cast<Opcode>(0x0024), s->is_paula());
            p.write<uint32_t>(static_cast<uint32_t>(s->hp_));
            s->send(p);
            ids.push_back(static_cast<uint16_t>(s->object_id()));
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        tail.push_back(static_cast<uint8_t>(e.result));
        const uint32_t n = static_cast<uint32_t>(ids.size());
        for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(n >> (8 * k)));
        for (uint16_t id : ids) { tail.push_back(static_cast<uint8_t>(id & 0xFF)); tail.push_back(static_cast<uint8_t>(id >> 8)); }
        e.end_ms  = INT32_MAX;
        e.life_ms = 0;
        LOG_DEBUG("GameClientSession %u: Wide Healing (ratio %.2f, radius %d): HP %d/%d, %u others healed", session_id(),
                  ratio, radius, hp_, hp_max_, n);
        break;
    }
    case EsKind::PartyDetect: {
        // sub_543490 (1425, loader sub_51D5C0: Ratio +4 float, Seconds +8, Radius +10): +296 = Seconds (the
        // lifetime 1000 * Seconds; its tick sub_543980 ends it there), +292 = Radius, +312 = the chosen point
        // (the payload's [u16 x][u16 y], ESAUI 6 — sub_C0BD20), the zone's characters (zone +28) of the same layer
        // in the caster's party (sub_716EF0: our share group, us included; none: the caster alone — no range, no
        // death test), counted (+304) with their ids in a set (+196: in id order), the reach (int)(DA * Ratio *
        // 0.1) (300 when the row's +0 > 0.9 — it is the constructor's 0.0); the tail (sub_543A40) [u16 n]{[u16 x]
        // [u16 y]} [u32 m]{u16 id} and, with n > 0, [u16 Seconds] (VERIFIED: the client's
        // ESAPartyItemDetectorScroll sub_51B360 reads that; it marks the points for the listed players). No TM.
        const float ratio = row_f("Ratio");
        const int radius  = static_cast<int16_t>(row_i("Radius"));
        e.end_ms  = 1000 * seconds;
        e.life_ms = e.end_ms;
        int16_t px = 0, py = 0;
        if (su_.payload.size() >= 4) {
            px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
            py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
        }
        const int reach = static_cast<int>(static_cast<double>(stat_value(7)) * ratio * 0.1000000014901161);
        const auto pts = detect_buried(px, py, radius, reach);
        const uint16_t n = static_cast<uint16_t>(pts.size());
        tail.push_back(static_cast<uint8_t>(n & 0xFF));
        tail.push_back(static_cast<uint8_t>(n >> 8));
        for (const auto& p : pts)
            for (int16_t v : { p.first, p.second }) {
                tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) & 0xFF));
                tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8));
            }
        std::vector<uint16_t> members;
        for (uint32_t oid : server_.entity_mgr().players_in_zone(zone_id_)) {
            GameClientSession* s = server_.player_session(oid);
            if (!s || !s->object_id_ || !s->in_world()) continue;
            if (s != this && !server_.party().same_group(char_uid_, s->char_uid_)) continue;
            members.push_back(static_cast<uint16_t>(s->object_id_));
        }
        std::sort(members.begin(), members.end());
        members.erase(std::unique(members.begin(), members.end()), members.end());
        const uint32_t m_n = static_cast<uint32_t>(members.size());
        for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(m_n >> (8 * k)));
        for (uint16_t id : members) {
            tail.push_back(static_cast<uint8_t>(id & 0xFF));
            tail.push_back(static_cast<uint8_t>(id >> 8));
        }
        if (n) {
            tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(seconds) & 0xFF));
            tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(seconds) >> 8));
        }
        LOG_DEBUG("GameClientSession %u: Party Detection at (%d,%d) radius %d reach %d: %u found", session_id(), px, py,
                  radius, reach, n);
        break;
    }
    case EsKind::Anniversary:
        // sub_576000 (1223, loader sub_51A360: Ratio +4, Seconds +8): +216 = Ratio, +220 = 1000 * Seconds,
        // +212 = 0 (the adder not in the chain yet); a target that is no player or has no drill in its
        // equipment slot 7 (sub_576150(7)) -> no object; the lifetime +220, S2C 224 with no tail
        // (sub_5EDCD0 + vtbl+48 = nothing; the client's ESAFirstAnniversary reads none), then the target's
        // motion stops (sub_41C5D0). No TM. Its tick (sub_5763A0) puts the adder in the chain past 1000 ms
        // (LK += (base + items) * Ratio, sub_576450) and sends LK's buff (sub_5761F0: recompute + S2C 283
        // [1][8][buff]); the end takes it out and sends it again.
        if (!equipped_drill()) {
            LOG_DEBUG("GameClientSession %u: 1st Anniversary needs a drill in slot 7 — no object", session_id());
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        e.ratio    = row_f("Ratio");
        e.end_ms   = 1000 * seconds;
        e.life_ms  = e.end_ms;
        e.stat     = 8;
        e.adder_on = false;
        break;
    case EsKind::SkillResist:
        // ESAPreventSlowDo (1328, sub_5808E0; ESAction_PreventSlow: Ratio +4 float, Second +8 i16, Radius
        // +10): +196 = (int16)(int)(Ratio * 100.0), +204 = 1000 * Second, +200 = 0, the lifetime +204, the tail
        // [u8 +180]; applied -> the target's skill resist (+1256) += +196 for (char)2215 (Dark Slow, sub_580A90
        // -> sub_763DD0) and the use TM. Its tick (sub_580C00) ends it at +204; the end takes the resist back
        // (sub_580B00). ESAInsulatorDo (1270, Strong Mind: 4301), ESAGasMaskDo (1313: 4010) and ESAInsulateArmorDo
        // (1314, Lightning Rod: 2113 and 5115) are the same class (sub_56E890 / sub_57DA30 / sub_57DF50; rows Ratio
        // +4, Second +8, Radius +10). (VERIFIED: the clients' ESAInsulatorDo 0x4F15B0 / ESAGasMask 0x4FE2B0 /
        // ESAInsulateArmor 0x4FEA70 / ESAPreventSlow 0x5020E0 read no tail — the [u8] goes unread.) A zone change
        // saves it; the restore (sub_580BB0 / sub_57DD00 / sub_57E250) adds the resist through the target ref
        // +208 that only the init sets (the ctor leaves it empty) — so 1328 / 1313 / 1314 add nothing then and take
        // nothing back at their end (retail); ESAInsulatorDo's (sub_56EB20) uses its owner +172: it adds it again.
        e.value    = static_cast<int16_t>(static_cast<int>(static_cast<double>(row_f("Ratio")) * 100.0));
        e.end_ms   = 1000 * static_cast<int16_t>(row_i("Second"));
        e.life_ms  = e.end_ms;
        e.adder_on = (e.result == 1);                      // the resist added (+208 set and applied)
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::PowerThrow:
        // ESAPowerThrow (1146, Precise Pitch; sub_558E40, ESAction_PowerThrow: Ratio +4, Seconds +8 i16): +196 =
        // Seconds, +8 = 0; a player caster: the skill's TM per hit on its target — the caster itself (sub_891C80) —
        // first; the lifetime 1000 * Seconds; the opener with no tail (VERIFIED: the client's ESAPowerThrow 0x4CE860
        // reads none). Its tick (sub_558F70) ends it at 1000 * Seconds. The throws read its Ratio while it is up.
        e.end_ms  = 1000 * seconds;
        e.life_ms = e.end_ms;
        skill_hit_tm_level(level_, e.skill);
        break;
    case EsKind::Hiding:
        // ESAHiding (1286, Hide; sub_570C00, ESAction_Hiding: Ratio +4, Second +8 i16): players only (else no
        // object); +196 = the player, +8 = 0, +220 = 1000 * Second, the lifetime; the opener with no tail (VERIFIED:
        // the client's ESAHiding 0x4F3820 reads none); then the skill's TM per hit on the player itself and the hide
        // (sub_570DA0). Its tick (sub_570D20): the player gone, or +8 past +220, the end; the end (sub_570F80): the
        // hide off. Its save keeps the base only and its restore (sub_571120) clears the player ref: it ends on the
        // next tick, no S2C 827.
        e.end_ms  = 1000 * static_cast<int16_t>(row_i("Second"));
        e.life_ms = e.end_ms;
        break;
    case EsKind::Mutation:
        // ESAWildMutation / ESABeastDo / ESAWildChange (1208 Evolution / 1494 Beast Instinct / 1248 Metamorphosis; sub_574730
        // / sub_5BAC80 / sub_575390; rows sub_51BE20 / sub_51E140: ApplyRatio +0, ApPRatio / AP_Ratio +4, DxPValue (int) /
        // DX_Ratio (float) +8, DpMRatio / DP_Ratio +12, HvMRatio / HV_Ratio +16, LkMRatio / LK_Ratio +20, HpRemainRatio +24,
        // Seconds +30, MpConUnit +32; sub_51C510: ApMRatio +4, DxMValue (int) +8, LkMRatio +12, DpPRatio +16, HvPRatio
        // +20, HpRemainRatio +24, Seconds +28, MpConUnit +30 — the ratios in mutation_formulas): +196 =
        // ApplyRatio, +200 = Seconds + 3, +204 = +8 = 0, the ratios kept, its caster (+208); ESABeastDo first ends our
        // ESAWildMutation (the limiter's vtbl+60(1208)) and is kept whatever the roll (+184); the tail [u8 +180] (VERIFIED:
        // the clients' ESAWildMutation 0x4EB420 / ESABeastDo 0x520770 read a u8); the lifetime 1000 * +200; then, applied,
        // the use TM and the mutation (mutation_start) — refused: no object, after the packets (retail). Its tick
        // (sub_575030 / sub_5BAF90): past 1000 * +200 the end; every 2000 ms (>) MpConUnit of MP (sub_575110 / sub_5BAF00:
        // not more MP than that -> the end; else MP -= it, S2C 0x0025); the base tick. Its end: mutation_end.
        if (d.esaction == 1494) es_remove_esaction(1208);
        e.end_ms   = 1000 * (static_cast<int16_t>(row_i("Seconds")) + 3);
        e.life_ms  = e.end_ms;
        e.ratio    = row_f("HpRemainRatio");
        e.mp_drop  = static_cast<int16_t>(row_i("MpConUnit"));
        e.pulse_ms = 0;
        e.stay     = (d.esaction == 1494);
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::SharpClaw: {
        // ESASharpClaw (1245, Fatal Wound; sub_569C10, loader sub_51C3A0: ApplyRatio +0, Ratio +4, ConRatio +8, Seconds +12):
        // +200 = Ratio, +196 = ApplyRatio, +204 = ConRatio, +208 = 1000 * Seconds, +8 = +216 = 0, its caster (+220); the
        // payload's target (kind 3, +244) we may attack (sub_5E6D70) — else no object; our mark (vtbl+132(target, 0)); the
        // first hit sub_560500 (AP' = (int)(AP * Ratio), ApplyRatio, element 0), its EXP share, +212 = its damage (the
        // bleed's base); its result 1 -> +180 = 2, 2 / 6 / 8 -> 3; the tail [u8 +180] (VERIFIED: the client's ESASharpClaw
        // 0x4E75C0 reads a u8); the lifetime +208; the block {the opener sub_5EDE00 [us][TARGET][ES id]..., the damage
        // event}; EXP 30003 kind 0 when we may attack it (vtbl+164); applied -> the skill's TM per hit. Its tick
        // (sub_56A0A0): past +208, us or the target gone, or its stop flag -> the end; every 2000 ms (>) the bleed
        // (sharp_claw_bleed) — a Type-13 target then takes the stop flag —; the base tick (not applied: the end).
        MonsterEntity* m = su_.target ? server_.entity_mgr().get_monster(su_.target) : nullptr;
        if (!m || m->zone != zone_id_ || !area_target_ok(*m)) {
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        e.target   = su_.target;
        e.ratio    = row_f("Ratio");
        e.base_ratio = row_f("ConRatio");
        e.end_ms   = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.life_ms  = e.end_ms;
        e.pulse_ms = 0;
        e.first    = false;                                // +268: the stop flag
        mark_monster_owner(*m);                            // vtbl+132(target, 0)
        const float apply = row_f("ApplyRatio");
        e.hp_ratio = apply;                                // +196 (the bleeds' roll)
        sc_hit = skill_phys_damage(*m, static_cast<int>(static_cast<double>(st_ap_) * static_cast<double>(e.ratio)), apply, 0);
        sc_hp_before = m->hp;
        sc_mob = m;
        e.value = sc_hit.dmg;                              // +212
        if (sc_hit.result == 1) e.result = 2;
        else if (sc_hit.result == 2 || sc_hit.result == 6 || sc_hit.result == 8) e.result = 3;
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    }
    case EsKind::Subcutaneous:
        // ESASubcutaneous (1311, Sumo Suit; sub_57D430, loader sub_51BEF0: ApplyRatio +0, Ratio +4, Second +8): +196 =
        // Ratio, +200 = Second, us as its owner (+172 / +204); the tail [u8 +180] (VERIFIED: the client's ESASubcutaneous
        // 0x4FDBF0 reads a u8), the lifetime 1000 * Second; applied -> the event 30015 to itself 1000 * Second later (its
        // end) and the use TM; not applied -> 30015 at once. Its damage handler (sub_57D6A0, mob_swing): a blow with an
        // attacker, without the flags 0x1000 / 0x2000 / 2, not of death type 4 / 5, loses (int)(damage * min(0.5,
        // (int)((our HV * Ratio + 20) / 3) / 100)) (sub_57D800); the flag 2 then.
        e.ratio   = row_f("Ratio");
        e.life_ms = 1000 * static_cast<int16_t>(row_i("Second"));
        e.end_ms  = (e.result == 1) ? e.life_ms : 0;
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::FluentWaist:
        // ESAFluentWaist (1166; sub_5608C0 — the action carries Dodge Master's row, ESAction_Soft_Waist sub_51A360:
        // ApplyRatio +0, Ratio +4, Seconds +8): +200 = 0, +196 = 1000 * Seconds, the lifetime +196; the opener
        // sub_5EDB30 [us][us][ES id]..., no tail (the client has no ESAFluentWaist: its factory sub_55CD40 makes
        // nothing for 1166 and the S2C 224 handlers skip a null, sub_6DE1E0 / sub_49DCE0); a player's use TM; then
        // (sub_560A00) we stop (sub_41C5D0) and take the condition 2048 — whatever the roll (the base tick ends a not
        // applied one). Its tick (sub_5609A0): +196, then the base tick. Its end (sub_560AC0): 2048 off when on.
        e.end_ms  = 1000 * seconds;
        e.life_ms = e.end_ms;
        break;
    case EsKind::WhiteKnight:
        // ESAWhiteKnight (1172, Bodyguard; sub_562840, loader sub_51B690: ApplyRatio +0, DamageRatio +4, Seconds +8, Radius
        // +10 i16, Comment +12): the row kept (+232), its caster (+208), +196 = 1000 * Seconds, +200 = +204 = 0, the
        // lifetime +196; the opener sub_5EDCD0, no tail (VERIFIED: the client's ESAWhiteKnight 0x4D7F10 reads none — its
        // "bodyguard" effect); a player's use TM whatever the roll. Its tick (sub_562A00): its caster gone or past +196
        // -> the end; every 400 ms (+204) the party walk (sub_562AC0, es_aura_pulse: each one of the zone in our party
        // — not us — on our layer within Radius gets the event 30149 [our id][lifetime] or ESAWhiteKnightDo 1173, sub_562C50
        // / sub_562D10: {1173, EState 0, ESClass 0, skill 4203, flags 2, this row, the lifetime}, us its caster); the
        // base tick.
        e.end_ms   = 1000 * seconds;
        e.life_ms  = e.end_ms;
        e.pulse_ms = 0;
        e.radius   = static_cast<int16_t>(row_i("Radius"));
        e.child    = 1173;
        break;
    case EsKind::WhiteKnightDo:
        // ESAWhiteKnightDo (1173, sub_562FF0; Bodyguard's row: DamageRatio +4): its caster (+236), +228 = DamageRatio, +8
        // = 0, the blow it keeps (+196, a DamageInfo) and its pending send (+224 / +232 / +260) cleared; the opener
        // sub_5EDCD0 [caster][us]... without a tail (VERIFIED: the client's ESAWhiteKnightDo sub_4D83C0 reads none — it plays
        // "bodyguard_2" and "snd_skl_p_109_sbg"); no TM. Its tick (sub_563150): its caster gone, or 2000 ms after the last
        // refresh (the event 30149 from that caster), the end. Its 30002 (sub_5631C0) halves our blows (mob_swing).
        e.ratio  = row_f("DamageRatio");
        e.end_ms = 2000;
        break;
    case EsKind::Kamikaze: {
        // ESAKamikaze (1479, Mass Destruction; sub_5B8360, loader sub_51DD60: ApplyRatio +0, Ratio +4, HpRemainRatio +8,
        // AfterBoomTime +12, PotionBanTime +16, KillNum +20, Comment +24, Radius +28): the row kept (+196), its owner
        // (+228), +8 = 0, kept whatever the roll (+184); the tail [u32 1000 * AfterBoomTime] (the client's ESAKamikaze —
        // patched, .myown 0x1272DB8 — reads none: its "boom-2" at once); the condition 8 (no skill but 1110, sub_890B30);
        // a player's use TM. No lifetime (+188 = 0: the 10 s warning on its first tick, ESClass 1). Its tick (sub_5B8510):
        // its owner gone -> the end; past 1000 * AfterBoomTime the boom (kamikaze_boom), the end; the base tick. Its end
        // (sub_5B8A00): the condition 8 off.
        e.stay       = true;
        e.end_ms     = 1000 * row_i("AfterBoomTime");
        e.ratio      = row_f("Ratio");
        e.base_ratio = row_f("HpRemainRatio");
        e.count      = row_i("KillNum");
        e.radius     = row_i("Radius");
        const uint32_t ms = static_cast<uint32_t>(e.end_ms);
        for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(ms >> (8 * k)));
        break;
    }
    case EsKind::KamikazeDo:
        // ESAKamikazeDo (1480; sub_5B8B90 — the action carries the Kamikaze's row): its owner (+200), +196 = 1000 *
        // PotionBanTime, +8 = 0, kept whatever the roll; no packet (no S2C 224 — the client has no class for 1480 —,
        // flags 1: no S2C 280 either); the condition 0x80000 (nothing in the server tests it alone — ESAUnableRecovery's
        // 0xC0000 sees it, retail). Its tick (sub_5B8C30): past +196 or its owner gone, the end; the base tick. Its end
        // (sub_5B8CA0): 0x80000 off.
        e.stay     = true;
        e.end_ms   = 1000 * row_i("PotionBanTime");
        no_opener  = true;
        break;
    case EsKind::FullHouse: {
        // ESAFullHouse (1280, Full House; sub_56F620, loader sub_51BF50: ApplyRatio +0, Ratio +4, Second +8, Radius +10,
        // Comment +12): the row kept (+220), its caster (+196), +8 = 0, kept whatever the roll; no Throw item (+4608) ->
        // no object; 5 of it used up (sub_894600); the tail [u8 +180][u16 the item's id][u64 its uid] (VERIFIED: the
        // client's ESAFullHouse 0x4F2600 reads them); applied -> a player's TM per hit on itself (sub_891C80). Its tick
        // (sub_56F860): see es_tick (full_house_mark / full_house_burst).
        const BagItem* tb = throw_item_ ? bag_find_uid(throw_item_) : nullptr;
        if (!tb) {
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        const uint32_t item = tb->item_id;
        const uint64_t uid  = tb->uid;
        consume_throw_item(5);
        e.stay   = true;
        e.radius = static_cast<int16_t>(row_i("Radius"));
        tail.push_back(static_cast<uint8_t>(e.result));
        tail.push_back(static_cast<uint8_t>(item));
        tail.push_back(static_cast<uint8_t>(item >> 8));
        for (int k = 0; k < 8; ++k) tail.push_back(static_cast<uint8_t>(uid >> (8 * k)));
        break;
    }
    case EsKind::GMCheer:
        // ESAGMCheerAttackDo / ESAGMCheerDefenceDo (1453 / 1456; sub_596D10, loader sub_51D700: ApUp +12, AcUp +14, MaUp +16,
        // MdUp +18, DaUp +20, LkUp +22, DpUp +24, HvUp +26, Seconds +28 (int), Radius +32 — the GM skills' LEVEL-1 row):
        // the eight kept, +212 = Seconds, +8 = 0, the lifetime 1000 * Seconds; the opener sub_5EDE00 [caster][us]..., no
        // tail (VERIFIED: the clients' ESAGMCheerAttackDo 0x51BCB0 / ESAGMCheerDefenceDo 0x51BFA0 read none); applied ->
        // the adder (sub_597310: skills 20000 / 20001 add each value, 20002 / 20003 base * value / 100) and the update
        // (gm_cheer_update). Its tick (sub_5971B0): past 1000 * Seconds the end; the base tick. Its end (sub_596EF0) and
        // its restore (sub_5970B0): the update. No TM.
        e.end_ms    = 1000 * row_i("Seconds");
        e.life_ms   = e.end_ms;
        e.bonus[0]  = static_cast<int16_t>(row_i("ApUp"));
        e.bonus[1]  = static_cast<int16_t>(row_i("AcUp"));
        e.bonus[4]  = static_cast<int16_t>(row_i("MaUp"));
        e.bonus[5]  = static_cast<int16_t>(row_i("MdUp"));
        e.bonus[7]  = static_cast<int16_t>(row_i("DaUp"));
        e.bonus[8]  = static_cast<int16_t>(row_i("LkUp"));
        e.bonus[10] = static_cast<int16_t>(row_i("DpUp"));
        e.bonus[11] = static_cast<int16_t>(row_i("HvUp"));
        e.value     = (d.id == 20002 || d.id == 20003) ? 1 : 0;   // the % mode
        break;
    case EsKind::FourCard:
        // ESAFourCardDo (1496; four_card_init): no Throw item -> no object.
        e.radius = row_i("Radius");
        if (!four_card_init(e, tail)) {
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        break;
    case EsKind::SharpSenseEx:
        // ESASharpSense_EX (1385, the Doctor's Sharp Sense; sub_58B3A0, loader sub_51BCC0: Ratio +4, Seconds +8): +196 =
        // Ratio, +200 = 1000 * Seconds, +8 = 0; NOT applied -> the init fails, no object, no packet; applied -> the tail
        // [u8 +180] (VERIFIED: the client's ESASharpSense_EX 0x514000 reads a u8), the lifetime +200, then Player +1068 =
        // Ratio (sub_58B560: the gun skills' crit multiplier, sub_5E53C0) and the use TM. Its end: +1068 = 1.0.
        if (e.result != 1) {
            server_.entity_mgr().free_oid(e.id);
            return false;
        }
        e.ratio   = row_f("Ratio");
        e.end_ms  = 1000 * seconds;
        e.life_ms = e.end_ms;
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::OpticalEyes:
        // ESAOpticalEyes (1289, Revealing Eye; sub_573C00, loader sub_51C020: Seconds +4 i16, Radius +6 i16 — no
        // ApplyRatio: always applied): +228 = 1000 * Seconds, +224 = Radius, its caster (+200), +232 = +236 = 0, the
        // seen list (+240, +496 = 0); it joins the caster's detectors (+456, sub_5E7830); the lifetime +228; the opener
        // with no tail (VERIFIED: the client's ESAOpticalEyes 0x4F7C10 reads none); the use TM (a player caster); then
        // the first look-around (optical_scan). Its tick (sub_574200): +232 / +236 += dt; past +228 the end; every 400
        // ms (+236) the look-around. Its end (sub_574310): S2C 828 [0][n][ids] — the seen ones hidden again —, out of
        // the detectors.
        e.end_ms   = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.radius   = static_cast<int16_t>(row_i("Radius"));
        e.life_ms  = e.end_ms;
        e.pulse_ms = 0;
        e.ids.clear();
        break;
    case EsKind::Bunker:
        // ESABunkerDo (1492; sub_5BA4C0, the Bunker row — loader sub_51E0D0: Seconds +4 int, Ratio +8, DefRatio +12):
        // +212 = 1000 * Seconds, +8 = 0, +228 = 0 (broken); its owner (+232); the shield +220 = (int)(15 * DA * Ratio)
        // = its max +216; +224 = (int)(15 * DA * DefRatio) — the adder's DP (sub_5BAC40); kept whatever the roll
        // (+184); the owner's condition 1 (sub_50D650: no MoveTo); the adder and the update (sub_5BABD0: recompute +
        // S2C 283 [1][10][DP buff]); the lifetime +212; the opener with the tail [u32 shield][u32 max] (VERIFIED: the
        // client's ESABunkerDo 0x51FEC0 reads two u32). Its damage handler (mob_swing): S2C 921 like ESAShield's. Its
        // tick (sub_5BA6D0): the owner gone, past +212 or broken -> the end; the end (sub_5BA970): the condition 1
        // off, the adder off + the update. Event 30015 ends it too.
        e.end_ms    = 1000 * row_i("Seconds");
        e.life_ms   = e.end_ms;
        e.value     = static_cast<int32_t>(static_cast<double>(15 * st_da_) * static_cast<double>(row_f("Ratio")));
        e.count     = e.value;
        e.bonus[10] = static_cast<int32_t>(static_cast<double>(15 * st_da_) * static_cast<double>(row_f("DefRatio")));
        e.stat      = 10;
        e.stay      = true;
        e.first     = false;
        e.adder_on  = true;
        pcond_add(1);
        for (int32_t v : { e.value, e.count })
            for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
        break;
    case EsKind::Mine: {
        // ESAMine / ESAMineMaster (1192 / 1267, Landmine / Explosives Expert; sub_565360 / sub_56DDD0; rows ApplyRatio
        // +0, TrapAp +4, Seconds +8 i16, Radius +10 i16): the row kept, +212 = the chosen point (the payload's [u16 x]
        // [u16 y]) — the object stands there —, its caster (+216, +248 = its id), +244 = 1000 * Seconds, +240 = 0; the
        // opener with the tail [pos the point][u16 Radius] (sub_565550 / sub_56DFB0; VERIFIED: the clients' ESAMine
        // 0x4DD460 / ESAMineMaster 0x4F0330 read the point — the Radius goes unread); then a player caster's TM per hit
        // on the owner — the caster (sub_891C80). Its tick: mine_tick.
        int16_t px = 0, py = 0;
        if (su_.payload.size() >= 4) {
            px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
            py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
        }
        e.fx      = px;
        e.fy      = py;
        e.radius  = static_cast<int16_t>(row_i("Radius"));
        e.end_ms  = 1000 * seconds;
        e.life_ms = 0;
        e.warned  = true;
        for (int16_t v : { px, py, static_cast<int16_t>(e.radius) }) {
            tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) & 0xFF));
            tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8));
        }
        break;
    }
    case EsKind::Aura:
        // ESAHeartBeat / ESAManaFastRecover / ESABigShield / ESAHawkEye (1125 / 1127 / 1129 / 1210, sub_555920 /
        // sub_556700 / sub_557880 / sub_567DD0; the rows: Ratio +4, Seconds +8 i16, Radius +10 i16): the row
        // kept, its caster, +196 = 1000 * Seconds, +200 = 0, the pulse +204 = 0, the lifetime +196. Not applied
        // -> S2C 0x01E4 [2][skill][0] to the caster (sub_5FD2E0) — ESAManaFastRecover then sends no opener;
        // ESAHawkEye has no such test. ESABigShield on a player: Team Bolster (4103) learned (sub_54BA80). The
        // opener S2C 224 has no tail (vtbl+48 writes none; VERIFIED: the client's ESAHeartBeat /
        // ESAManaFastRecover / ESABigShield / ESAHawkEye read none). ESAHawkEye alone gives the use TM. Its tick
        // (sub_555B40 / sub_556930 / sub_557B10 / sub_567F90): past +196 the end; every 400 ms the pulse.
        e.ratio    = row_f("Ratio");
        e.end_ms   = 1000 * seconds;
        e.life_ms  = e.end_ms;
        e.radius   = static_cast<int16_t>(row_i("Radius"));
        e.child    = d.esaction + 1;                       // 1126 / 1128 / 1130 / 1211 / 1176
        e.pulse_ms = 0;
        // (ESAGoodLuck, Aura of Luck — sub_563780: the row copied, its caster, +196 = 1000 * Seconds, the pulse
        // timer +204 = 0, no tail (VERIFIED: the client's ESAGoodLuck 0x4D8E30 reads none), no message when not
        // applied, the use TM whatever the roll.)
        if (e.result != 1 && d.esaction != 1210 && d.esaction != 1212 && d.esaction != 1152 && d.esaction != 1175)
            send_system_msg(2, e.skill, 0);
        if (d.esaction == 1127 && e.result != 1) no_opener = true;
        if (d.esaction == 1129) e.bolster = find_skill(4103) != nullptr;
        break;
    case EsKind::AuraChild:
        // ESAHeartBeatDo / ESAManaFastRecoverDo / ESABigShieldDo (1126 / 1128 / 1130, sub_556110 / sub_556FA0 /
        // sub_558190; the action carries the aura's row and its remaining lifetime +188): active (+216 = 1),
        // the row's Ratio, its caster, +8 = 0, the StatusValueAdder in its chain — 1126 AP (stat chain 9), 1128
        // the recovery amount (chain 2, MP), 1130 DP (chain 9) — and the update (1126 sub_556260 / 1130 sub_558310:
        // recompute + S2C 283 [1][stat][buff]); the opener without a tail (VERIFIED: the client's ESAHeartBeatDo /
        // ESAManaFastRecoverDo read none). Its tick (sub_556320 / sub_5571E0 / sub_558420): 2000 ms after the last
        // refresh, or with its caster gone, the end.
        e.ratio      = row_f("Ratio");
        e.base_ratio = e.ratio;
        e.end_ms     = 2000;
        e.adder_on   = true;
        e.count      = 0;
        // (ESAGoodLuckDo 1176, sub_563F10: active, +212 = the row's Ratio, its caster, +244 = the row's +10, +8 = 0,
        // the adder — LK += (base + items) * Ratio (sub_5640D0) — and the update sub_564060 (recompute + S2C 283
        // [1][8][LK buff]); no tail (VERIFIED: the client's ESAGoodLuckDo 0x4D91F0 reads none). Its tick sub_564120:
        // the caster gone, or 2000 ms after the last refresh (event 30154 [caster id][lifetime]), the end.)
        e.stat       = (d.esaction == 1126) ? 0 : (d.esaction == 1130) ? 10 : (d.esaction == 1211) ? 1 :
                       (d.esaction == 1213) ? 2 : (d.esaction == 1176) ? 8 : -1;
        if (d.esaction == 1213) e.value = row_i("DX");    // ESAMultiAgilityUpDo sub_5677F0: +212 = the row's DX (int)
        if (d.esaction == 1153) {
            // ESACheerUpDo (sub_55A490): +216 = Ratio / 100, +220 = Radius / 70, the tail [f32 +220 * 100] (VERIFIED:
            // the client's ESACheerUpDo 0x4CF620 reads a float); its adder is a DiscountAmountHandler (mp_discount),
            // no stat.
            e.ratio = static_cast<float>(static_cast<double>(row_f("Ratio")) / 100.0);
            const float r70 = static_cast<float>(static_cast<double>(static_cast<int16_t>(row_i("Radius"))) / 70.0);
            const float shown = static_cast<float>(static_cast<double>(r70) * 100.0);
            uint32_t bits;
            std::memcpy(&bits, &shown, sizeof(bits));
            for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(bits >> (8 * k)));
        }
        break;
    case EsKind::MagicReflect:
        // ESAMagicReflect (1097, sub_54DCC0; ESAction_Magic_Reflect: Ratio +4, Seconds +8 i16): +196 = Ratio,
        // +200 = Seconds, the lifetime 1000 * Seconds, the tail [u8 +180]; applied -> the event 30015 to itself
        // 1000 * Seconds later (its end) and the use TM; not applied -> 30015 at once. While it is up, a damage
        // event of death type 1 / 3 on its target not reflected yet (flags & 1) gives back (int)(damage *
        // min(0.8, (HV / 100 + 0.3) * Ratio)) to the attacker (sub_54DF20 / sub_54E140) — monsters deal no
        // such hit here yet (no monster skills).
        e.ratio   = row_f("Ratio");
        e.end_ms  = (e.result == 1) ? 1000 * seconds : 0;
        e.life_ms = 1000 * seconds;
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::Dash: {
        // ESADash (1142, sub_55C0B0; ESAUI 3: a point): its caster (+204); a caster under the condition 15
        // (stunned) -> no object (players take none here); +228 = the point (the payload's [u16 x][u16 y]), the
        // tail [u16 x][u16 y] (VERIFIED: the client's ESADash sub_4D1770 reads a position), the use TM. Its tick
        // (sub_55C250): the first one starts the run (sub_55C320), then it lives while the run goes on
        // (sub_55C460), at most 2000 ms.
        int16_t px = 0, py = 0;
        if (su_.payload.size() >= 4) {
            px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
            py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
        }
        e.value   = static_cast<int32_t>((static_cast<uint32_t>(static_cast<uint16_t>(py)) << 16) |
                                         static_cast<uint16_t>(px));
        e.end_ms  = INT32_MAX;
        e.life_ms = 0;
        e.stay    = true;
        e.adder_on = false;                                  // (+196: the run not started yet)
        for (int16_t v : { px, py }) {
            tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) & 0xFF));
            tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8));
        }
        break;
    }
    case EsKind::Guard: {
        // ESAGuard (1309, sub_5B5BE0; ESAction_Guard: Count +4 i16, Seconds +6 i16): +224 = 1000 * Seconds,
        // +220 = +222 = Count, its target (the caster); kept whatever the apply roll (sub_5275E0); the tail
        // [u16 Count] (VERIFIED: the client's ESAGuard sub_4FA730 reads it); no TM. Our damage events then go
        // through it (sub_5B5E10, es_guard_hit); its tick (sub_5B5D70) ends it at +224 or with Count gone.
        // Party Guard (4707, ESAction 1528 — the Bear, a reconstruction, skill_bear.cpp) is this class on us and on
        // our party: its row's Gnum blows each, 1000 * Seconds ms, tail [u32 ms] (VERIFIED: the client's
        // ESAPartyGuardDo sub_524160 reads a u32 — its time).
        const int16_t cnt = static_cast<int16_t>(row_i(d.esaction == 1528 ? "Gnum" : "Count"));
        e.value   = cnt;
        e.end_ms  = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.life_ms = e.end_ms;
        e.stay    = true;
        if (d.esaction == 1528) {
            for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(e.end_ms) >> (8 * k)));
        } else {
            tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(cnt) & 0xFF));
            tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(cnt) >> 8));
        }
        break;
    }
    case EsKind::DeadAlive:
        // ESADeadAlive (1351, sub_587AE0; ESAction_DeadAlive: Ratio +4, HPPercent +8 i16, Seconds +12 i16 — no
        // ApplyRatio: always applied): +212 = Ratio, +216 = Seconds, +248 = HPPercent / 100.0, +256 = the same
        // (sub_587D00), inactive (+244 = 0), the last check +264 = 0; the tail [u8 result] (VERIFIED: the
        // client's ESADeadAlive sub_510DF0 reads it); the lifetime 1000 * Seconds; applied -> its adder joins
        // the chain (AP += (base + items) * (Ratio - HP ratio * 2.5) while active) and the use TM. Its tick
        // (sub_5880B0): every 3000 ms the HP ratio: above the threshold -> inactive (+ the AP update), else
        // active (+ the update).
        e.ratio     = row_f("Ratio");
        e.end_ms    = 1000 * static_cast<int16_t>(row_i("Seconds"));
        e.life_ms   = e.end_ms;
        e.threshold = static_cast<double>(static_cast<int16_t>(row_i("HPPercent"))) / 100.0;
        e.hp_ratio  = e.threshold;
        e.adder_on  = false;
        e.last_ms   = 0;
        e.stat      = 0;
        tail.push_back(static_cast<uint8_t>(e.result));
        break;
    case EsKind::Berserk:
        // ESABerserk (1171, sub_561930; ESAction_Berserk: Seconds +4 i16, ApRatio +8, Radius +12 i16, HpDrop +14
        // i16, MpDrop +16 i16 — no ApplyRatio: always applied): +212 = 1000 * Seconds, the timers 0, the
        // condition 1256 on the target (8 | 32 | 64 | 128 | 1024: no skill, move, attack request, pick-up, drill
        // — sub_50D650), its adder in the chain WITHOUT a recompute (AP += base * ApRatio - base, base > 0: it
        // counts from the next recompute on, retail), the lifetime, the opener without a tail (VERIFIED: the
        // client's ESABerserk sub_4D7A90 reads none) and the use TM.
        e.ratio   = row_f("ApRatio");
        e.end_ms  = 1000 * seconds;
        e.life_ms = e.end_ms;
        e.radius  = static_cast<int16_t>(row_i("Radius"));
        e.hp_drop = static_cast<int16_t>(row_i("HpDrop"));
        e.mp_drop = static_cast<int16_t>(row_i("MpDrop"));
        e.pulse_ms = 0;
        e.last_ms  = 0;
        e.idle_ms  = 0;
        e.seek_ms  = 0;
        e.stat     = 0;
        pcond_add(1256);
        break;
    default:
        server_.entity_mgr().free_oid(e.id);
        return false;
    }

    // S2C 224 (sub_5EDE00): the caster is the target here — to the area around us. (ESAStatUpScroll
    // sends none.)
    const uint16_t self = static_cast<uint16_t>(object_id_);
    struct TbHit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<TbHit> tb_hits;                            // ESAThunderBolt's hits, their aftermath below
    std::vector<CounterHit> fc_hits;                       // ESAFourCardDo's hits (in its block), their aftermath below
    if (kind != EsKind::ScrollShell && !no_opener) {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(caster ? caster : self);         // (another player's skill on us: [it][us])
        o.write<uint16_t>((kind == EsKind::FlashCut || kind == EsKind::ThunderBolt || kind == EsKind::SharpClaw)
                          ? e.target : self);              // [us][target]
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(static_cast<uint16_t>(e.esaction));
        o.write<uint8_t>(kEsKind);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        for (uint8_t b : tail) o.write<uint8_t>(b);
        group.push_back(std::move(o));
        if (kind == EsKind::SharpClaw && sc_mob) sc_killed = skill_damage_event(*sc_mob, sc_hit, group);   // sub_569C10
        if (kind == EsKind::FourCard && e.first) four_card_hits(e, act, group, fc_hits);                    // sub_5BBB10
        if (kind == EsKind::ThunderBolt || kind == EsKind::WindSlash || kind == EsKind::SkyRay) {   // sub_55F030 /
            const bool tb = (kind == EsKind::ThunderBolt);                                             //   sub_55D8C0 /
            const bool sky = (kind == EsKind::SkyRay);                                                 //   sub_570870
            const int32_t base = row_i("AP") + (sky ? 0 : 3 * elem_slot(tb ? 4 : 2));
            const float apply  = row_f("ApplyRatio");
            for (size_t k = 0; k < e.ids.size(); ++k) {
                MonsterEntity* m = server_.entity_mgr().get_monster(e.ids[k]);
                if (!m) continue;
                const SkillDamage h = skill_magic_damage(*m, base, apply, (tb || sky) ? 25 : 49, sky ? 32 : tb ? 16 : 4);
                const int32_t hp_before = m->hp;
                const bool killed = skill_damage_event(*m, h, group);
                // Hardening (not retail): retail marks a fixed +196 array; ours is a vector each init sizes to ids.
                if (h.dmg > 0 && k < e.hit.size()) e.hit[k] = 1;
                tb_hits.push_back({ m, h.dmg, hp_before, killed });
            }
        }
        server_.broadcast_block(zone_id_, group, 0);
    }
    // The limiter's add (vtbl+44 sub_5E1EB0 -> sub_5E14B0): a free entry of the list (ESAction 1000), else
    // none — the ES stays out of the limiter. (Only when an ES made by another's init took the entry its
    // admission freed: ESAStatUpScroll after its Do.)
    if (e.limited) {
        const int list = es_list(e.esclass);
        int used = 0;
        for (const ActiveEs& o : es_) if (o.limited && es_list(o.esclass) == list) ++used;
        if (list < 0 || list >= kEsLists || used >= kPlayerEsCapacity[list]) e.limited = false;
    }
    es_.push_back(e);
    LOG_DEBUG("GameClientSession %u: ES %u ESAction %d (skill %u lv %u) result %d ratio %.2f value %d %d ms",
              session_id(), e.id, e.esaction, e.skill, e.level, e.result, e.ratio, e.value, e.end_ms);
    if (kind == EsKind::ThirdEye) third_eye_on();          // event 30280

    if (kind == EsKind::PowerThrow) return true;           // (its TM came before the opener)
    if (kind == EsKind::Hiding) {                          // sub_570C00: the TM per hit on the player, then the hide
        skill_hit_tm_level(level_, e.skill);
        hide_set(true);
        return true;
    }
    if (kind == EsKind::Mine) {                            // a player caster's TM per hit on the owner (itself)
        skill_hit_tm_level(level_, e.skill);
        return true;
    }
    if (kind == EsKind::Aura && d.esaction == 1175) {      // ESAGoodLuck: the use TM whatever the roll
        skill_use_tm(e.skill);
        return true;
    }
    if (kind == EsKind::OpticalEyes) {                     // sub_573C00: the use TM, then the first look-around
        skill_use_tm(e.skill);
        for (ActiveEs& o : es_) if (o.id == e.id) { optical_scan(o); break; }
        return true;
    }
    if (kind == EsKind::Bunker) {                          // the adder in, the update (no TM: the handler's)
        es_stat_update(10);
        return true;
    }
    if (kind == EsKind::SharpClaw) {                       // EXP 30003 when we may attack it; applied: the TM per hit
        if (sc_mob)
            skill_after_hit(*sc_mob, sc_hit.dmg, sc_hp_before, sc_killed, (e.result == 1) ? e.skill : 0,
                            !may_attack_owned(*sc_mob), std::chrono::steady_clock::now(), 0);
        return true;
    }
    if (kind == EsKind::GMCheer) {                         // applied: the adder in, the update
        if (e.result == 1) gm_cheer_update();
        return true;
    }
    if (kind == EsKind::WhiteKnight) {                     // sub_562840: the use TM whatever the roll
        skill_use_tm(e.skill);
        return true;
    }
    if (kind == EsKind::Kamikaze) {                        // sub_5B8360: the condition 8, the use TM
        pcond_add(8);
        skill_use_tm(e.skill);
        return true;
    }
    if (kind == EsKind::KamikazeDo) {                      // sub_5B8B90: the condition 0x80000
        pcond_add(0x80000);
        return true;
    }
    if (kind == EsKind::HammerWheel) {                     // (skill_bear.cpp) our speed while it turns, the use TM
        set_current_speed(spin_speed_);
        skill_use_tm(e.skill);
        return true;
    }
    if ((d.esaction == 1528 || d.esaction == 1544) && caster == 0) {
        // Party Guard / Best Shield (the Bear): the same ES on each member of our share group (skill_bear.cpp)
        int32_t v = 0;
        if (d.esaction == 1544) {
            int32_t eq[12];
            equip_bonus(eq);
            v = static_cast<int32_t>(static_cast<double>(eq[10]) * static_cast<double>(row_f("PDpRatio")) / 100.0);
        }
        bear_party_spread(d, level, v);
        if (d.esaction == 1528) skill_use_tm(e.skill);     // (the KR Guard gives none; a Bear buff does)
    }
    if (kind == EsKind::FullHouse) {                       // applied: the TM per hit on ourselves
        if (e.result == 1) skill_hit_tm_level(level_, e.skill);
        return true;
    }
    if (kind == EsKind::FourCard) {                        // the hits' aftermath; all four: the TM per hit on ourselves
        const auto now = std::chrono::steady_clock::now();
        for (const CounterHit& h : fc_hits) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 0);
        if (e.first) skill_hit_tm_level(level_, e.skill);
        return true;
    }
    if (kind == EsKind::FluentWaist) {                     // sub_5608C0: the use TM, then sub_560A00
        skill_use_tm(e.skill);
        skill_stop_motion(std::chrono::steady_clock::now());
        pcond_add(2048);
        return true;
    }
    if (kind == EsKind::Subcutaneous) {                    // applied: the use TM
        if (e.result == 1) skill_use_tm(e.skill);
        return true;
    }
    if (kind == EsKind::Mutation) {                        // applied: the use TM, then the mutation (refused: no object)
        if (e.result == 1) {
            skill_use_tm(e.skill);
            for (size_t k = 0; k < es_.size(); ++k) {
                if (es_[k].id != e.id) continue;
                if (!mutation_start(es_[k], act)) {
                    server_.entity_mgr().free_oid(es_[k].id);
                    es_.erase(es_.begin() + static_cast<std::ptrdiff_t>(k));
                    LOG_DEBUG("GameClientSession %u: mutation %d refused — no object", session_id(), e.esaction);
                }
                break;
            }
        }
        return true;
    }
    if (kind == EsKind::Anniversary) {                     // sub_41C5D0 on the target; no adder, no TM yet
        skill_stop_motion(std::chrono::steady_clock::now());
        return true;
    }
    if (kind == EsKind::ThunderBolt || kind == EsKind::WindSlash) {   // EXP 30003 kind 1 (in the block, retail)
        const auto now = std::chrono::steady_clock::now();
        for (const TbHit& h : tb_hits) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 1);
        if (kind == EsKind::WindSlash) { skill_use_tm(e.skill); return true; }   // the use TM, whatever the roll
        if (MonsterEntity* t = server_.entity_mgr().get_monster(e.target)) skill_hit_tm(*t, e.skill);   // per hit
        return true;
    }
    if (kind == EsKind::Field || kind == EsKind::ElemArea) {   // the use TM whatever the roll (sub_53DBF0 / sub_589040)
        skill_use_tm(e.skill);
        return true;
    }
    if (kind == EsKind::HolyShield) return true;           // (sub_526E70: no TM)
    if (kind == EsKind::SkyRay) {                          // sub_570570: EXP 30003 kind 1 (in the block, retail)
        const auto now = std::chrono::steady_clock::now();
        for (const TbHit& h : tb_hits) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 1);
        if (e.result == 1) {
            protect(3000);                                 // sub_73CF80(3000, 0)
            skill_use_tm(e.skill);
        }
        return true;
    }
    if (kind == EsKind::SettleDown) return true;           // (no TM: Refreshing Dew's handler gave it)
    if (kind == EsKind::DarkUp) {                          // sub_5BA1F0(1); no TM
        elem_es_add(6, static_cast<int16_t>(e.value));
        return true;
    }
    if (kind == EsKind::LightShield) {                     // sub_56CBC0: the adder, LightR, S2C 283, the TM — whatever
        elem_es_add(16, static_cast<int16_t>(e.value));    //   the roll
        es_light_shield_update();
        skill_use_tm(e.skill);
        return true;
    }
    if (kind == EsKind::Bind) {
        if (e.result == 1) {                               // sub_534170: applied
            skill_stop_motion(std::chrono::steady_clock::now());
            pcond_add(10);
            skill_use_tm(e.skill);
            bind_children(es_.back());
            for (ActiveEs& o : es_) if (o.id == e.id) { o.first = true; break; }   // +308
        }
        return true;
    }
    if (kind == EsKind::ElementUp) {                       // sub_5B75D0(1) whatever the roll; no TM
        element_up_apply(e, true);
        return true;
    }
    if (kind == EsKind::WhirlWind) {                       // the use TM whatever the roll (sub_565EB0)
        skill_use_tm(e.skill);
        return true;
    }
    if (kind == EsKind::FlashCut) {                        // the use TM whatever the roll (sub_573320)
        skill_use_tm(e.skill);
        return true;
    }
    // Applied: register the StatusValueAdder (sub_7172E0), recompute + S2C 283 (vtbl+160 and
    // sub_5ED310), the skill's TM (sub_891DC0).
    if (e.result == 1) {
        // (ESAStaticShield only joins the chain: no recompute and no 283 at its start or end,
        // sub_554BE0 / sub_554DF0 — its DP counts from the next recompute on)
        if (e.stat >= 0 && d.esaction != 1118 && kind != EsKind::DeadAlive && kind != EsKind::Berserk)
            es_stat_update(e.stat);
        if (kind == EsKind::ScrollStatUp) es_scroll_update(e);
        if (kind == EsKind::SharpSense) power_shot_mult_ = e.ratio;
        if (kind == EsKind::SharpSenseEx) gun_skill_mult_ = e.ratio;   // sub_58B560
        if (kind == EsKind::Boiling) cdm_.boost = e.ratio;
        if (kind == EsKind::SkillResist)                   // sub_580A90 / sub_56EA20 / sub_57DBE0 / sub_57E100 (1)
            for (int s : skill_resist_ids(e.esaction)) skill_resist_add(s, e.value);
        if (kind == EsKind::ElemShield) elem_es_add(e.count, static_cast<int16_t>(e.value));   // sub_56B190(1)
        if (kind == EsKind::DarkBarrier) elem_es_add(17, static_cast<int16_t>(e.value));       // sub_56D540(1)
        // (no TM from: ESAFastLoad, ESAPartyItemDetectorScroll, the auras — but ESAHawkEye — and their children)
        // The TM goes to the init's caster (sub_891DC0 on a4): another player's skill on us -> to it.
        if (kind != EsKind::FastLoad && kind != EsKind::PartyDetect && kind != EsKind::AuraChild &&
            kind != EsKind::Guard && kind != EsKind::SelfCure && kind != EsKind::WhiteKnightDo &&
            (kind != EsKind::Aura || d.esaction == 1210 || d.esaction == 1212 || d.esaction == 1152)) {
            if (caster == 0) skill_use_tm(e.skill);
            else if (GameClientSession* c = server_.player_session(caster)) c->skill_use_tm_later(e.skill);
        }
        if (d.esaction == 1095 && find_skill(4202)) {
            // ESAHVUp's init (sub_54B880), a player with Super Hips (4202) learned: sub_54BAD0 — the action {ESAction
            // 1166 ESAFluentWaist, EState 0, ESClass 0, flags 2, skill 4202, Dodge Master's row} on us through
            // sub_524690(.., 1): 4202 is no Type 2 -> the limiter's admission, then the object.
            if (const SkillDef* sh = server_.skill_db().get(4202)) {
                SkillDef dd = *sh;
                dd.esaction = 1166;
                dd.estate   = 0;
                dd.esclass  = 0;
                if (dd.type != 2 && dd.id != 7046) {
                    if (es_limiter_admit(0, 1166, 4202)) es_create(dd, level, act, EsKind::FluentWaist, true, 2);
                } else {
                    es_create(dd, level, act, EsKind::FluentWaist, false, 2);
                }
            }
        }
    }
    return true;
}

// ── ESAStatUpScroll's init (sub_58D700): the 1388 action on the caster through sub_524690(.., 0) ──
//  {ESAction 1388, EState 0, the same ESClass, flags 2, the 1387 skill and its row}; the skill is not of
//  Type 2 -> the limiter's admission (a debuff class: by the skill) and the limiter; no scroll use-up.
void GameClientSession::es_scroll_do(const SkillDef& d, uint8_t level, const EsaRow* act)
{
    SkillDef dd = d;
    dd.esaction = 1388;
    dd.estate   = 0;
    if (dd.esclass == -1) { es_create(dd, level, act, EsKind::ScrollStatUp, false, 2); return; }
    if (d.type != 2 && d.id != 7046) {
        if (!es_limiter_admit(dd.esclass, dd.esaction, static_cast<uint16_t>(dd.id))) return;
        es_create(dd, level, act, EsKind::ScrollStatUp, true, 2);
        return;
    }
    es_create(dd, level, act, EsKind::ScrollStatUp, false, 2);
}

// ESAStatUpScrollDo's update (sub_58E210): the target's vtbl+160 (recompute) and S2C 283 [u8 n]{[u8 stat]
// [u32 value]} with each stat that has a bonus: its buff (+656 + 4i) — MD's is read from +580, the stat
// itself (retail; no scroll of this data has MdUpRatio).
void GameClientSession::es_scroll_update(const ActiveEs& e)
{
    recompute_stats(true);
    uint8_t n = 0;
    PacketBuffer body(static_cast<Opcode>(0x011B), is_paula());
    std::vector<std::pair<uint8_t, int32_t>> v;
    for (int i = 0; i < 12; ++i)
        if (e.bonus[i] > 0) v.emplace_back(static_cast<uint8_t>(i), i == 5 ? stat_value(5) : buff_[i]);
    n = static_cast<uint8_t>(v.size());
    body.write<uint8_t>(n);
    for (const auto& s : v) { body.write<uint8_t>(s.first); body.write<uint32_t>(static_cast<uint32_t>(s.second)); }
    send(body);
}

// sub_524690 (a4 = 1) / sub_8911A0 after an ESAction with its own handler: a made ES of a debuff class
// (4 / 5 / 6) on a player's skill uses up the item at Player +0xC94 when it is a SubType 69 one —
// Player::UseItem (vtbl+144); refused: the log.
void GameClientSession::scroll_use_up(int esclass, int esaction)
{
    if (!es_debuff_class(esclass) || !scroll_uid_) return;
    BagItem* b = bag_find_uid(scroll_uid_);
    const ItemDef* it = b ? server_.item_db().get(b->item_id) : nullptr;
    if (!b || !it || it->sub_type != 69) return;
    if (!use_item(*b, *it))
        LOG_INFO("SKILL_SCROLL USE FALSE %s %u %d", char_name_, it->id, esaction);
}

// ── each ES's vtbl+56 on the zone tick ────────────────────────────────────────
//  The classes' tick: elapsed (+8) += dt; past their end -> vtbl+28; else the base tick
//  sub_50FF90: not applied and not kept -> vtbl+28; the lifetime (+188) -= dt, at <= 10000 S2C 850
//  once (ESClass not 3 / -1).
// Hardening (not retail): retail's loop ticks on regardless; a throw here is logged so world_tick still re-arms.
void GameClientSession::es_tick(int elapsed_ms)
try {
    std::vector<uint16_t> pulses;                          // the auras whose 400 ms came (run after the loop:
                                                           //   a pulse may make or end ESs)
    for (size_t i = 0; i < es_.size();) {
        ActiveEs& e = es_[i];
        if ((e.kind == EsKind::AuraChild || e.kind == EsKind::Hiding) && e.orphan) { es_end(i); continue; }   // its
                                                                                                    //   ref is gone
        if ((e.kind == EsKind::AuraChild || e.kind == EsKind::WhiteKnightDo) &&      // sub_556320 / sub_563150: a
            e.caster != static_cast<uint16_t>(object_id_)) {                         //   member's aura / Bodyguard
            GameClientSession* c = server_.player_session(e.caster);                 //   whose caster left the zone
            if (!c || !c->in_world() || c->zone_id() != zone_id_) { es_end(i); continue; }
        }
        if (e.kind == EsKind::Mine) {                      // sub_565600 / sub_56E060
            if (mine_tick(i, elapsed_ms)) ++i;
            continue;
        }
        if (e.kind == EsKind::WhiteKnight) {               // sub_562A00
            e.elapsed_ms += elapsed_ms;
            e.pulse_ms   += elapsed_ms;
            if (e.elapsed_ms >= e.end_ms) { es_end(i); continue; }
            if (e.pulse_ms >= 400) { e.pulse_ms = 0; pulses.push_back(e.id); }   // sub_562AC0 (es_aura_pulse)
            if (es_base_tick(i, elapsed_ms)) ++i;
            continue;
        }
        if (e.kind == EsKind::Kamikaze) {                  // sub_5B8510
            e.elapsed_ms += elapsed_ms;
            if (e.orphan) { es_end(i); continue; }
            if (e.elapsed_ms < e.end_ms) {
                if (es_base_tick(i, elapsed_ms)) ++i;
                continue;
            }
            e.elapsed_ms = 0;
            const ActiveEs copy = e;
            kamikaze_boom(copy);                           // (may kill us: OnDie ends the lists 0 / 1 / 7 with it)
            for (size_t k = 0; k < es_.size(); ++k)
                if (es_[k].id == copy.id) { es_end(k); break; }
            continue;
        }
        if (e.kind == EsKind::KamikazeDo) {                // sub_5B8C30
            e.elapsed_ms += elapsed_ms;
            if (e.elapsed_ms >= e.end_ms || e.orphan) { es_end(i); continue; }
            if (es_base_tick(i, elapsed_ms)) ++i;
            continue;
        }
        if (e.kind == EsKind::FullHouse) {                 // sub_56F860
            e.elapsed_ms += elapsed_ms;
            if (e.orphan) { es_end(i); continue; }
            full_house_mark();                             // the server-wide spot, when unset
            if (es_[i].elapsed_ms < 1000) {
                if (es_base_tick(i, elapsed_ms)) ++i;
                continue;
            }
            es_[i].elapsed_ms = 0;
            const ActiveEs copy = es_[i];
            full_house_burst(copy);
            for (size_t k = 0; k < es_.size(); ++k)
                if (es_[k].id == copy.id) { es_end(k); break; }
            continue;
        }
        if (e.kind == EsKind::FourCard) {                  // sub_5BBAA0
            e.elapsed_ms += elapsed_ms;
            if (e.elapsed_ms >= 1000) { es_end(i); continue; }
            if (es_base_tick(i, elapsed_ms)) ++i;
            continue;
        }
        if (e.kind == EsKind::Bunker && e.first) {         // sub_5BA6D0: broken -> +220 = +224 = 0, the end
            e.value = 0;
            e.bonus[10] = 0;
            es_end(i);
            continue;
        }
        if (e.kind == EsKind::Mutation) {                  // sub_575030 / sub_5BAF90
            e.elapsed_ms += elapsed_ms;
            e.pulse_ms   += elapsed_ms;
            if (e.elapsed_ms >= e.end_ms || e.orphan) { es_end(i); continue; }
            if (e.pulse_ms > 2000) {
                e.pulse_ms = 0;
                if (mp_ <= e.mp_drop) { es_end(i); continue; }   // sub_575110: not more MP than MpConUnit
                mp_ -= e.mp_drop;
                PacketBuffer p(static_cast<Opcode>(0x0025), is_paula());   // sub_5F49B0
                p.write<uint32_t>(static_cast<uint32_t>(mp_));
                send(p);
            }
        }
        if (e.kind == EsKind::SharpClaw) {                 // sub_56A0A0
            e.elapsed_ms += elapsed_ms;
            if (e.elapsed_ms >= e.end_ms) { es_end(i); continue; }
            MonsterEntity* m = e.target ? server_.entity_mgr().get_monster(e.target) : nullptr;
            if (e.orphan || !m || m->zone != zone_id_ || e.first) { es_end(i); continue; }
            e.pulse_ms += elapsed_ms;
            if (e.pulse_ms > 2000) {
                sharp_claw_bleed(e);
                es_[i].pulse_ms = 0;
                if (const MobDef* md = server_.mob_db().get(static_cast<int>(m->mob_id)))
                    if (md->type == 13) es_[i].first = true;
            }
        }
        if (es_[i].kind == EsKind::Mutation || es_[i].kind == EsKind::SharpClaw) {   // then the base tick (sub_50FF90)
            ActiveEs& b = es_[i];
            if (b.result != 1 && !b.stay) { es_end(i); continue; }
            if (!b.warned) {
                b.life_ms -= elapsed_ms;
                if (b.life_ms <= kEsWarnMs) {
                    if (b.esclass != 3 && b.esclass != -1) {
                        PacketBuffer p(static_cast<Opcode>(0x0352), is_paula());
                        p.write<uint16_t>(static_cast<uint16_t>(b.esaction));
                        send(p);
                    }
                    b.life_ms = 0;
                    b.warned  = true;
                }
            }
            ++i;
            continue;
        }
        if (e.kind == EsKind::OpticalEyes) {               // sub_574200
            e.elapsed_ms += elapsed_ms;
            e.pulse_ms   += elapsed_ms;
            if (e.elapsed_ms >= e.end_ms) { es_end(i); continue; }
            if (e.pulse_ms >= 400) { e.pulse_ms = 0; optical_scan(e); }
            ++i;
            continue;
        }
        if (e.kind == EsKind::Dash) {
            // ESADash's tick (sub_55C250). The first: sub_55C320 — farther than d² 30 from the point (sub_41ED00):
            // MoveTo the point with our move bits (+92) as the type (the move strategy takes that type's speed,
            // S2C 0x0018), then the current speed 600 (vtbl+92: S2C 0x01D8); our attack target is let go
            // (sub_717DB0). The next ones: the run must go on — d² >= 30, moving, speed 600 (sub_55C460) — else
            // we stop (sub_41C5D0) and it ends; past 2000 ms it ends.
            const auto now = std::chrono::steady_clock::now();
            const int16_t px = static_cast<int16_t>(e.value & 0xFFFF);
            const int16_t py = static_cast<int16_t>((static_cast<uint32_t>(e.value) >> 16) & 0xFFFF);
            int32_t cx, cy;
            cur_pos(now, cx, cy);
            const int dx = px - static_cast<int16_t>(cx), dy = py - static_cast<int16_t>(cy);
            const double d2 = static_cast<double>(dy * dy + dx * dx);
            if (e.adder_on) {
                const bool running = d2 >= 30.0 && motion_.moving(now) && cur_speed_ == 600.0f;
                if (!running) { skill_stop_motion(now); es_end(i); continue; }
            } else {
                if (d2 > 30.0) es_run_to(px, py, 600.0f, now);
                if (attacking_) end_attack(true);
                es_[i].adder_on = true;
            }
            es_[i].elapsed_ms += elapsed_ms;
            if (es_[i].elapsed_ms > 2000) { es_end(i); continue; }
            ++i;
            continue;
        }
        if (e.kind == EsKind::ThunderBolt) {
            // ESAThunderBolt's tick (sub_55F210; no base tick): past 1000 ms once, a player caster: the Electroshock
            // follow-up (sub_55F300) — with Electroshock (2113) learned, each target that took damage (+196) gets
            // its ESAElectricShockDo (sub_55F480: {1183, EState 0, ESClass 1, flags 2, skill 2113, its learned row}
            // after the DebuffSet roll, through sub_524690(.., 1)); past 4000 ms the end.
            e.elapsed_ms += elapsed_ms;
            if (e.elapsed_ms > 1000 && !e.shock_done) {
                e.shock_done = true;
                const std::vector<uint16_t> ids = e.ids;
                const std::vector<uint8_t>  hit = e.hit;
                if (find_skill(2113))
                    for (size_t k = 0; k < ids.size(); ++k)
                        if (k < hit.size() && hit[k])
                            if (MonsterEntity* m = server_.entity_mgr().get_monster(ids[k]))
                                if (m->zone == zone_id_) passive_follow_es(*m, 2113, 1183);
            }
            if (es_[i].elapsed_ms > 4000) { es_end(i); continue; }
            ++i;
            continue;
        }
        if (e.kind == EsKind::SaintOath) {                 // sub_58BB00
            e.value -= elapsed_ms;
            if (e.value < 0) { e.value = 0; es_end(i); continue; }
            if (e.result != 1 && !e.stay) { es_end(i); continue; }   // the base tick
            ++i;
            continue;
        }
        if (e.kind == EsKind::DarkWhisper) {               // sub_58BF90
            const auto now = std::chrono::steady_clock::now();
            e.value        = static_cast<int16_t>(e.value - elapsed_ms);
            e.interval_acc = static_cast<int16_t>(e.interval_acc - elapsed_ms);
            int32_t cx, cy;
            cur_pos(now, cx, cy);
            if (static_cast<int16_t>(e.value) <= 0 || static_cast<int16_t>(cx) != e.fx || static_cast<int16_t>(cy) != e.fy) {
                es_[i].value = 0;
                es_end(i);
                continue;
            }
            if (static_cast<int16_t>(e.interval_acc) <= 0) {
                const uint16_t id = e.id;
                dark_whisper_pulse(i);
                if (i >= es_.size() || es_[i].id != id) continue;
                es_[i].interval_acc = 2000;
            }
            ActiveEs& w = es_[i];
            if (w.result != 1 && !w.stay) { es_end(i); continue; }   // the base tick
            if (!w.warned) {
                w.life_ms -= elapsed_ms;
                if (w.life_ms <= kEsWarnMs) {
                    PacketBuffer p(static_cast<Opcode>(0x0352), is_paula());
                    p.write<uint16_t>(static_cast<uint16_t>(w.esaction));
                    send(p);
                    w.life_ms = 0;
                    w.warned  = true;
                }
            }
            ++i;
            continue;
        }
        if (e.kind == EsKind::Bind) {                      // sub_5349F0
            const auto now = std::chrono::steady_clock::now();
            {                                              // sub_534AB0: a dead one's ESABindDo ends, leaves the list
                std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
                for (size_t k = 0; k < e.ids.size();) {
                    MonsterEntity* m = server_.entity_mgr().get_monster(e.ids[k]);
                    if (m && m->alive && m->hp > 0) { ++k; continue; }
                    if (m)
                        for (size_t j = 0; j < m->es.size(); ++j)
                            if (m->es[j].esaction == 1155) { server_.monster_es_end(*m, j); break; }
                    e.ids.erase(e.ids.begin() + static_cast<std::ptrdiff_t>(k));
                }
            }
            e.elapsed_ms += elapsed_ms;
            if (e.elapsed_ms >= e.end_ms || motion_.moving(now)) { es_end(i); continue; }
            if (e.result != 1 && !e.stay) { es_end(i); continue; }   // the base tick
            if (!e.warned) {                               // (no lifetime: the warning at once)
                e.life_ms -= 2 * elapsed_ms;               //   (its base tick runs twice while we stand)
                if (e.life_ms <= kEsWarnMs) {
                    PacketBuffer p(static_cast<Opcode>(0x0352), is_paula());
                    p.write<uint16_t>(static_cast<uint16_t>(e.esaction));
                    send(p);
                    e.life_ms = 0;
                    e.warned  = true;
                }
            }
            ++i;
            continue;
        }
        if (e.kind == EsKind::DarkBarrier && e.first && e.other) {   // sub_56D7E0 case 2: the 200 ms pending blow
            e.interval_acc += elapsed_ms;
            if (e.interval_acc > 200) {
                int32_t d[7];
                for (int k = 0; k < 7; ++k) d[k] = e.dinfo[k];
                const uint16_t mob = e.other;
                e.interval_acc = 0;
                e.first = false;
                e.other = 0;
                if (MonsterEntity* m = server_.entity_mgr().get_monster(mob))
                    if (m->zone == zone_id_) barrier_send_back(*m, d, nullptr, nullptr);
                if (i >= es_.size()) break;
            }
        }
        if (es_[i].kind == EsKind::HolyShield) {           // sub_527050
            ActiveEs& h = es_[i];
            h.elapsed_ms += elapsed_ms;
            if (h.elapsed_ms >= h.end_ms) h.first = true;
            if (h.first) { es_end(i); continue; }
            h.interval_acc += elapsed_ms;
            if (h.interval_acc > h.interval_ms) {
                h.interval_acc = 0;
                if (h.value < h.count) {
                    h.value += static_cast<int32_t>(static_cast<double>(h.count) * 0.1);
                    if (h.value > h.count) h.value = h.count;
                }
                PacketBuffer p(static_cast<Opcode>(0x0399), is_paula());
                p.write<uint16_t>(h.id);
                p.write<int32_t>(h.value);
                p.write<int32_t>(h.count);
                server_.broadcast_zone(zone_id_, p, 0);
            }
            if (h.result != 1 && !h.stay) { es_end(i); continue; }   // the base tick
            ++i;
            continue;
        }
        if (es_[i].kind == EsKind::BloodyOath) {           // sub_586560
            ActiveEs& b = es_[i];
            b.value -= elapsed_ms;
            if (b.value < 0) { b.value = 0; es_end(i); continue; }
            if (b.result != 1 && !b.stay) { es_end(i); continue; }   // the base tick
            ++i;
            continue;
        }
        if (es_[i].kind == EsKind::SaintPromise) {         // sub_585FC0
            ActiveEs& sp = es_[i];
            sp.count        -= elapsed_ms;
            sp.interval_acc -= elapsed_ms;
            if (sp.interval_acc < 0) {                     // sub_586050
                if (hp_ < hp_max_) hp_ = std::min(hp_ + sp.value, hp_max_);
                PacketBuffer p(static_cast<Opcode>(0x0024), is_paula());
                p.write<uint32_t>(static_cast<uint32_t>(hp_));
                send(p);
                sp.interval_acc = 3000;
            }
            if (sp.count < 0) { sp.count = 0; es_end(i); continue; }
            if (sp.result != 1 && !sp.stay) { es_end(i); continue; }   // the base tick
            ++i;
            continue;
        }
        if (e.kind == EsKind::Field || e.kind == EsKind::ElemArea) {   // sub_53DEB0 / sub_5892B0
            e.elapsed_ms += elapsed_ms;
            e.pulse_ms   += elapsed_ms;
            if (e.elapsed_ms >= e.end_ms) { es_end(i); continue; }
            if (e.pulse_ms >= 1000) {
                e.pulse_ms = 0;
                const uint16_t id = e.id;
                if (e.kind == EsKind::Field) field_pulse_es(i);
                else elem_area_pulse(i);
                if (i >= es_.size() || es_[i].id != id) continue;
            }
            if (es_[i].result != 1 && !es_[i].stay) { es_end(i); continue; }   // the base tick
            ++i;
            continue;
        }
        if (e.kind == EsKind::GaleBreak) {                 // sub_586D50
            e.end_ms -= elapsed_ms;
            if (e.end_ms < 0 && !e.first) {
                gale_break_hit(es_[i]);
                es_[i].first = true;
            } else if (e.first) {
                e.pulse_ms += elapsed_ms;
                if (e.pulse_ms > 3000) { es_end(i); continue; }
            }
            ++i;
            continue;
        }
        if (e.kind == EsKind::HammerWheel) {               // Hammer Spin (skill_bear.cpp): its time, a hit every TicTime
            e.elapsed_ms += elapsed_ms;
            e.pulse_ms   += elapsed_ms;
            if (e.elapsed_ms >= e.end_ms) { es_end(i); continue; }
            if (e.value > 0 && e.pulse_ms >= e.value) {
                e.pulse_ms = 0;
                const ActiveEs copy = e;
                hammer_wheel_pulse(copy);
            }
            ++i;
            continue;
        }
        if (e.kind == EsKind::WhirlWind) {                 // sub_566140
            const auto now = std::chrono::steady_clock::now();
            e.elapsed_ms += elapsed_ms;
            e.pulse_ms   += elapsed_ms;
            if (e.elapsed_ms >= e.end_ms || motion_.moving(now)) { es_end(i); continue; }
            if (e.pulse_ms >= 2000) {
                e.pulse_ms = 0;
                const ActiveEs copy = e;
                whirlwind_pulse(copy);
            }
            if (es_[i].result != 1 && !es_[i].stay) { es_end(i); continue; }   // the base tick
            ++i;
            continue;
        }
        if (e.kind == EsKind::FlashCut) {
            // ESAFlashAttacking's tick (sub_573600; ESAPowerFlashAttacking sub_5883C0: 750 for 600, and first our
            // condition 15 -> vtbl+36: it goes silently — no S2C 280, no vtbl+100). We or the target gone -> the
            // end. The first: farther than d² 80² from the spot (sub_41ED00 / sub_41ED80) -> MoveTo it with our
            // move bits, then the current speed 600 / 750. The next ones: within 80 -> we stop (sub_41C5D0), the
            // roll sub_893840 on the row's ApplyRatio, the hit (ESAction 1276 through the switch, sub_573880 /
            // sub_5885F0: {1276, EState 0, ESClass -1, flags 2, skill 1302 / 1304, the row, the target}), the end;
            // else standing (our move bits 0, sub_41C820) or another speed -> the end.
            const auto now = std::chrono::steady_clock::now();
            const float speed = e.power ? 750.0f : 600.0f;
            if (e.power && pcond_test(15)) {
                server_.entity_mgr().free_oid(e.id);
                es_.erase(es_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            const MonsterEntity* t = server_.entity_mgr().get_monster(e.target);
            if (!t || t->zone != zone_id_) { es_end(i); continue; }
            const int16_t px = static_cast<int16_t>(e.value & 0xFFFF);
            const int16_t py = static_cast<int16_t>((static_cast<uint32_t>(e.value) >> 16) & 0xFFFF);
            int32_t cx, cy;
            cur_pos(now, cx, cy);
            const int outside = cmp_r(dist2(static_cast<int16_t>(cx), static_cast<int16_t>(cy), px, py), 80);
            if (e.first) {
                if (outside >= 0) es_run_to(px, py, speed, now);
                es_[i].first = false;
                ++i;
                continue;
            }
            if (outside < 0) {
                skill_stop_motion(now);
                const SkillDef* sd = server_.skill_db().get(e.skill);
                const EsaRow* row = sd ? server_.esa_db().row(sd->esa_table, e.level) : nullptr;
                const bool applied = apply_chance(row ? static_cast<float>(row->get("ApplyRatio", 0.0)) : 0.0f);
                const ActiveEs copy = e;
                esa_flash_hit(copy, applied);
                size_t k = 0;
                while (k < es_.size() && es_[k].id != copy.id) ++k;
                if (k < es_.size()) es_end(k);
                else ++i;
                continue;
            }
            if (!motion_.moving(now) || cur_speed_ != speed) { es_end(i); continue; }
            ++i;
            continue;
        }
        e.elapsed_ms += elapsed_ms;
        if (e.kind == EsKind::Aura && e.elapsed_ms < e.end_ms) {
            e.pulse_ms += elapsed_ms;
            if (e.pulse_ms >= 400) { e.pulse_ms = 0; pulses.push_back(e.id); }
        }
        if (e.kind == EsKind::Anniversary && !e.adder_on && e.elapsed_ms > 1000) {   // sub_5763A0
            e.adder_on = true;
            es_stat_update(8);                             // sub_5761F0: recompute + S2C 283 [1][8][LK buff]
        }
        if (e.countdown ? (e.elapsed_ms > e.end_ms) : (e.elapsed_ms >= e.end_ms)) {
            // ESAThirdEye's tick (sub_58EB60) posts the event 30281 before its end — only here, at
            // its natural end (a replaced or cleared Mind's Eye ends without it, retail).
            if (e.kind == EsKind::ThirdEye) third_eye_off();
            es_end(i);
            continue;
        }
        if (e.kind == EsKind::EnergySword && !equip_[1]) { es_end(i); continue; }   // sub_588C80: slot 1 emptied
        if (e.kind == EsKind::SelfCure) {                  // sub_5B96B0: every Interval, HP += Heal (sub_718F20)
            e.interval_acc += elapsed_ms;
            if (e.interval_acc >= e.interval_ms && e.interval_ms > 0) {
                e.interval_acc -= e.interval_ms;
                hp_ = std::min(hp_ + e.value, hp_max_);
                PacketBuffer p(static_cast<Opcode>(0x0024), is_paula());
                p.write<uint32_t>(static_cast<uint32_t>(hp_));
                send(p);
            }
        }
        if (e.kind == EsKind::Berserk) {                   // sub_561BF0 (past its end: ended above)
            e.pulse_ms += elapsed_ms;
            e.last_ms  += elapsed_ms;
            e.seek_ms  += elapsed_ms;
            if (e.pulse_ms > 100) {
                e.pulse_ms = 0;
                if (!es_berserk_seek(es_[i], elapsed_ms)) { es_end(i); continue; }
            }
            if (es_[i].last_ms > 1000) {
                es_[i].last_ms = 0;
                if (!es_berserk_drain(es_[i])) { es_end(i); continue; }
            }
        }
        if (es_[i].kind == EsKind::DeadAlive && es_[i].elapsed_ms - es_[i].last_ms > 3000) {   // sub_5880B0
            ActiveEs& da = es_[i];
            da.last_ms  = da.elapsed_ms;
            da.hp_ratio = (hp_max_ > 0) ? static_cast<double>(hp_) / static_cast<double>(hp_max_) : 0.0;
            if (da.threshold < da.hp_ratio) {
                if (da.adder_on) { da.adder_on = false; es_stat_update(0); }
            } else {
                da.adder_on = true;
                es_stat_update(0);                         // sub_587DB0: recompute + S2C 283 [1][0][AP buff]
            }
        }
        if (e.result != 1 && !e.stay) { es_end(i); continue; }
        if (!e.warned) {
            e.life_ms -= elapsed_ms;
            if (e.life_ms <= kEsWarnMs) {
                if (e.esclass != 3 && e.esclass != -1) {
                    PacketBuffer p(static_cast<Opcode>(0x0352), is_paula());
                    p.write<uint16_t>(static_cast<uint16_t>(e.esaction));
                    send(p);
                }
                e.life_ms = 0;
                e.warned  = true;
            }
        }
        ++i;
    }
    for (uint16_t id : pulses)
        for (size_t i = 0; i < es_.size(); ++i)
            if (es_[i].id == id) { es_aura_pulse(i); break; }
} catch (const std::exception& ex) {
    LOG_ERROR("GameClientSession %u: es_tick: %s", session_id(), ex.what());
} catch (...) {
    LOG_ERROR("GameClientSession %u: es_tick: unknown exception", session_id());
}

// sub_50FF90 on es_[i]: not applied and not kept -> the end; the 10 s warning (S2C 850 [u16 ESAction] to us, sub_604360)
// once, ESClass not 3 / -1. False: it ended.
bool GameClientSession::es_base_tick(size_t i, int elapsed_ms)
{
    ActiveEs& b = es_[i];
    if (b.result != 1 && !b.stay) { es_end(i); return false; }
    if (!b.warned) {
        b.life_ms -= elapsed_ms;
        if (b.life_ms <= kEsWarnMs) {
            if (b.esclass != 3 && b.esclass != -1) {
                PacketBuffer p(static_cast<Opcode>(0x0352), is_paula());
                p.write<uint16_t>(static_cast<uint16_t>(b.esaction));
                send(p);
            }
            b.life_ms = 0;
            b.warned  = true;
        }
    }
    return true;
}

// ── An aura's pulse, every 400 ms of its tick (ESAHeartBeat sub_555C00, ESAManaFastRecover sub_5569F0,
//    ESABigShield sub_557BD0, ESACheerUp sub_559F20, ESAGoodLuck sub_563A00, ESAMultiAgilityUp sub_5672E0,
//    ESAHawkEye sub_568050) ──────────────────────────────────────────────────────────────────────────────
//  The zone's players (zone +28) in the caster's party (sub_716EF0: its PartyMemberJudge — our share group, us
//  included; none: ourselves alone) on its layer (sub_893F00) within the row's Radius (sub_42F1B0) — no other
//  test (a dead member too). ESACheerUp walks them only with its caster standing (sub_41C820 on +92); moving, the
//  caster alone. ESABigShield first collects them and a 7th found ends the pulse with nobody done (its array holds
//  6 — a party holds 6, so never). ESAWhiteKnight's walk (Bodyguard, sub_562AC0) is the same but skips its caster (by
//  id). Each one found (aura_child_from, on its strand) gets the event, or the child.
void GameClientSession::es_aura_pulse(size_t i)
{
    if (i >= es_.size()) return;
    const ActiveEs aura = es_[i];
    const auto now = std::chrono::steady_clock::now();
    const uint16_t self = static_cast<uint16_t>(object_id_);
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    std::vector<GameClientSession*> found;
    if (aura.esaction == 1152 && motion_.moving(now)) {
        found.push_back(this);
    } else {
        for (uint32_t oid : server_.entity_mgr().players_in_zone(zone_id_)) {
            GameClientSession* s = server_.player_session(oid);
            if (!s || !s->object_id_ || !s->in_world()) continue;
            if (s != this && !server_.party().same_group(char_uid_, s->char_uid_)) continue;
            if (s == this && aura.kind == EsKind::WhiteKnight) continue;
            int32_t x, y;
            s->cur_pos(now, x, y);
            if (!(static_cast<double>(dist_f(x, y, cx, cy)) <= static_cast<double>(aura.radius))) continue;
            found.push_back(s);
            if (aura.esaction == 1129 && found.size() > 6) return;
        }
    }
    // ESABigShield's count: the members found with Team Bolster, else 0.
    const int32_t count = (aura.esaction == 1129 && aura.bolster) ? static_cast<int32_t>(found.size()) : 0;
    for (GameClientSession* s : found) {
        if (s == this) aura_child_from(self, aura.skill, aura.level, aura.child, aura.life_ms, count, zone_id_);
        else s->aura_child_pulse(self, aura.skill, aura.level, aura.child, aura.life_ms, count, zone_id_);
    }
}

// ESAWhiteKnightDo's damage event (sub_5631C0, 30002): with its caster still there (+236), a blow with damage and none
// of the DamageInfo's flags (+16): we keep (int)(damage * 0.5); the bodyguard's part, (int)(that * DamageRatio), goes
// out as the event 30157 [our id][the DamageInfo] (death type 1 / 3 at once, else 200 ms later: +224 / +232 / +260) —
// which no handler of the server takes (Player / Character / the ESs): it is lost (retail).
void GameClientSession::whiteknight_cut(const ActiveEs& e, int32_t& dmg, int32_t flags)
{
    if (flags != 0 || dmg <= 0) return;
    GameClientSession* c = server_.player_session(e.caster);
    if (!c || !c->in_world() || c->zone_id() != zone_id_) return;
    dmg = static_cast<int32_t>(static_cast<double>(dmg) * 0.5);
}

// sub_58B9F0: our share group's players (sub_7692C0 -> sub_86D0F0: the processor's six slots in order, ours among
// them — the players of this server by their uids) on our layer (sub_893F00) within `radius` of us, at most `max` (no
// other test: a dead one too). No group (no PartyMemberJudge, +1160): none — not even us.
std::vector<GameClientSession*> GameClientSession::party_in_radius(int32_t radius, size_t max)
{
    std::vector<GameClientSession*> out;
    const PartyShare sh = server_.party().share_of(char_uid_);
    if (!sh.in_group || max == 0) return out;
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    std::vector<GameClientSession*> zone;
    for (uint32_t oid : server_.entity_mgr().players_in_zone(zone_id_))
        if (GameClientSession* s = server_.player_session(oid))
            if (s->object_id_ && s->in_world()) zone.push_back(s);
    for (const PartyShare::Slot& slot : sh.members) {
        GameClientSession* s = nullptr;
        for (GameClientSession* z : zone) if (z->char_uid_ == slot.uid) { s = z; break; }
        if (!s) continue;
        int32_t x, y;
        s->cur_pos(now, x, y);
        if (static_cast<double>(dist_f(cx, cy, x, y)) <= static_cast<double>(radius)) out.push_back(s);
        if (out.size() >= max) break;
    }
    return out;
}

// A party ESAction's action on us (from ourselves, or a party member's skill in `zone`): 1386 — Sacred Oath's {ESAction
// 1386, EState 0, ESClass 0, flags 18, the skill, its row} through sub_524690(.., 1), we its caster (the action's ref
// set to us); 1388 — a party stat scroll's ESAStatUpScrollDo (es_scroll_do, sub_524690(.., 0)), we its caster; 1487 —
// Refreshing Dew's ESASettleDownDo {ESAction 1487, EState 0, ESClass 0, flags 2, skill 2505, its row} through
// sub_524690(.., 1) (sub_5DFB60), `caster` its caster (0: us); 1328 / 1270 / 1313 / 1314 — Leg Training's / Strong
// Mind's / Gas Mask's / Lightning Rod's ES {it, EState 0, ESClass 0, flags 2, the skill, its row} through
// sub_524690(.., 1) (sub_5D8CE0 and the like), `caster` its caster.
void GameClientSession::party_action_from(uint16_t caster, uint16_t skill, uint8_t level, int esaction, int16_t zone)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld || zone_id_ != zone || !object_id_) return;
    const SkillDef* sd = server_.skill_db().get(skill);
    if (!sd) return;
    const EsaRow* act = server_.esa_db().row(sd->esa_table, level);
    if (esaction == 1388) { es_scroll_do(*sd, level, act); return; }
    SkillDef dd = *sd;
    dd.esaction = esaction;
    dd.estate   = 0;
    dd.esclass  = 0;
    const uint16_t by = (caster == static_cast<uint16_t>(object_id_)) ? 0 : caster;
    if (esaction == 1328 || esaction == 1270 || esaction == 1313 || esaction == 1314) {
        if (dd.type != 2 && dd.id != 7046) {
            if (!es_limiter_admit(dd.esclass, dd.esaction, skill)) return;
            es_create(dd, level, act, EsKind::SkillResist, true, 2, 0, by);
            return;
        }
        es_create(dd, level, act, EsKind::SkillResist, false, 2, 0, by);
        return;
    }
    if (!es_limiter_admit(0, esaction, skill)) return;
    if (esaction == 1487) {
        es_create(dd, level, act, EsKind::SettleDown, true, 2, 0, by);
        return;
    }
    es_create(dd, level, act, EsKind::SaintPromise, true, 18);
}

// The pulse on us (from ourselves, or a member of the caster's party in `zone`): a child of that ESAction in our
// limiter (sub_75FD70) takes the event 30133 / 30134 / 30135 / 30138 / 30154 / 30214 / 30215 [caster id] (ESABigShield
// + [count]) [the aura's lifetime] (sub_556390 / sub_557250 / sub_558490 / sub_55A7D0 / sub_564190 / sub_5687E0 /
// sub_567A60; Bodyguard's ESAWhiteKnightDo 30149, sub_5631C0): the lifetime (+188); ESABigShieldDo the count (+212
// against +216: a change sets its Ratio = the row's + {0.03, 0.06, 0.12, 0.24, 0.48}[count] for 2..6 (flt_98FAE4) and
// updates DP, sub_558310); and from its own caster (its ref's id) active again with its 2000 ms restarted. None: the
// child (sub_555E30 / sub_556C20 / sub_557EB0 / sub_55A180 / sub_563C30 / sub_567510 / sub_568280 / sub_562D10: {the
// child's ESAction, EState 0, ESClass 0, the aura's skill, flags 2, the aura's row, its lifetime}, its caster the
// aura's) through sub_524690(.., 1) — the limiter admits it (the skill no Type 2), then it is made.
void GameClientSession::aura_child_from(uint16_t caster, uint16_t skill, uint8_t level, int child, int32_t life_ms,
                                        int32_t count, int16_t zone)
{
    static const float kBolster[7] = { 0.0f, 0.0f, 0.03f, 0.06f, 0.12f, 0.24f, 0.48f };
    if (leaving_ || state_ != GameClientSessionState::InWorld || zone_id_ != zone || !object_id_) return;
    const EsKind kind = (child == 1173) ? EsKind::WhiteKnightDo : EsKind::AuraChild;
    for (ActiveEs& c : es_) {
        if (!c.limited || c.esaction != child) continue;
        c.life_ms = life_ms;
        if (child == 1130 && c.count != count) {
            c.count = count;
            c.ratio = c.base_ratio;
            if (count > 1 && count < 7) c.ratio = c.ratio + kBolster[count];
            es_stat_update(10);
        }
        if (!c.orphan && c.caster == caster) {
            if (kind == EsKind::AuraChild) c.adder_on = true;
            c.elapsed_ms = 0;
        }
        return;
    }
    const SkillDef* sd = server_.skill_db().get(skill);
    if (!sd) return;
    SkillDef dd = *sd;
    dd.esaction = child;
    dd.estate   = 0;
    dd.esclass  = 0;
    const EsaRow* row = server_.esa_db().row(sd->esa_table, level);
    const uint16_t by = (caster == static_cast<uint16_t>(object_id_)) ? 0 : caster;
    if (dd.type != 2 && dd.id != 7046) {
        if (!es_limiter_admit(0, child, skill)) return;
        es_create(dd, level, row, kind, true, 2, life_ms, by);
        return;
    }
    es_create(dd, level, row, kind, false, 2, life_ms, by);
}

// ESABerserk's seek (sub_561CD0, on its 100 ms pulse). With an attack target (the holder +232): every 1000 ms
// of +228, the target within Radius -> the event 30107 [it][2][0]. Without one: the first monster of the list
// (not another player's — but our party's —, on our layer, not Type 8 / 12, a shadow one only with the Third
// Eye) within Radius of us -> 30107 [it][2][0]; none: standing (+92) adds the pulse's time to +224 — 3000
// ends it —, moving clears it. (PvP players first: no PvP here.) False: it ends.
bool GameClientSession::es_berserk_seek(ActiveEs& e, int elapsed_ms)
{
    const auto now = std::chrono::steady_clock::now();
    int32_t px, py;
    cur_pos(now, px, py);
    if (attacking_ && atk_target_) {
        if (e.seek_ms >= 1000) {
            e.seek_ms = 0;
            int32_t tx, ty;
            if (skill_target_pos(atk_target_, now, tx, ty) &&
                static_cast<double>(dist_f(px, py, tx, ty)) <= static_cast<double>(e.radius))
                skill_follow_attack(atk_target_, 0);
        }
        return true;
    }
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0) continue;
        if (owned_by_other(*m)) continue;
        if (!area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (static_cast<double>(dist_f(px, py, mx, my)) <= static_cast<double>(e.radius)) {
            skill_follow_attack(static_cast<uint16_t>(oid), 0);
            return true;
        }
    }
    if (!motion_.moving(now)) {
        e.idle_ms += elapsed_ms;
        if (e.idle_ms >= 3000) return false;
    } else {
        e.idle_ms = 0;
    }
    return true;
}

// ESABerserk's drain (sub_562450, every 1000 ms of +220), not while dead: HP - HpDrop and MP - MpDrop; either
// at or below its drop -> it ends (nothing taken); else both and S2C 0x0024 / 0x0025.
bool GameClientSession::es_berserk_drain(ActiveEs& e)
{
    if (dead_) return true;
    int32_t hp = hp_ - e.hp_drop;
    if (hp > hp_) hp = hp_;
    if (hp <= e.hp_drop) return false;
    int32_t mp = mp_ - e.mp_drop;
    if (mp > mp_) mp = mp_;
    if (mp <= e.mp_drop) return false;
    hp_ = hp;
    mp_ = mp;
    PacketBuffer a(static_cast<Opcode>(0x0024), is_paula());
    a.write<uint32_t>(static_cast<uint32_t>(hp_));
    send(a);
    PacketBuffer b(static_cast<Opcode>(0x0025), is_paula());
    b.write<uint32_t>(static_cast<uint32_t>(mp_));
    send(b);
    return true;
}

// ResChain 2 (sub_717030): the HP (0) / MP (1) recovery amount — the rate, plus each ESAManaFastRecoverDo
// active with a Ratio above 0: (int)(rate * Ratio) for MP (sub_557100), plus PlayerRecoveryAmountAcc
// (Player +2724, sub_739710) while a pet item put it in: (int)(rate * +2740) for HP / (int)(rate * +2744) for
// MP when that value is above 0 (pet.cpp).
int32_t GameClientSession::regen_amount(int kind, int32_t rate) const
{
    int32_t acc = rate;
    if (pet_regen_on_) {
        const float r = (kind == 0) ? pet_regen_hp_ : pet_regen_mp_;
        if (r > 0.0f) acc += static_cast<int32_t>(static_cast<double>(rate) * r);
    }
    for (const ActiveEs& e : es_)
        if (e.kind == EsKind::AuraChild && e.esaction == 1128 && kind == 1 && e.adder_on && e.ratio > 0.0f)
            acc += static_cast<int32_t>(static_cast<double>(rate) * e.ratio);
    return acc;
}

// The DiscountAmountHandler chain of kind 1 (Character +1136, sub_717290) on a skill's MpCon: each
// ESACheerUpDo (Bolster Ballad's child, sub_55A8B0) — whatever its active flag — makes it (int)(cost * Ratio /
// 100) when our MP covers that, else the original cost.
int32_t GameClientSession::mp_discount(int32_t cost) const
{
    int32_t v = cost;
    for (const ActiveEs& e : es_) {
        if (e.kind != EsKind::AuraChild || e.esaction != 1153) continue;
        const int32_t d = static_cast<int32_t>(static_cast<double>(v) * e.ratio);
        v = (mp_ - d >= 0) ? d : cost;
    }
    return v;
}

// ── vtbl+28 (sub_50F770): S2C 280, the class's vtbl+100, out of the world ────────────────
void GameClientSession::es_end(size_t i)
{
    if (i >= es_.size()) return;
    const ActiveEs e = es_[i];
    es_.erase(es_.begin() + static_cast<std::ptrdiff_t>(i));
    server_.entity_mgr().free_oid(e.id);                   // the ES object leaves: its id goes back
    if (e.flags & 2) {
        PacketBuffer p(static_cast<Opcode>(0x0118), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<uint16_t>(e.id);
        if (e.flags & 4) send(p);                          // sub_5EE220: the target only
        else server_.broadcast_zone(zone_id_, p, 0);       // sub_5EE2A0 / sub_5EE170: the area
    }
    // vtbl+100: an applied StatusValueAdder leaves the chain (sub_7174A0) -> recompute + S2C 283;
    // ESASharpSense puts +1064 back to 1.0 (sub_568D20).
    if (e.result == 1 && e.stat >= 0 && e.esaction != 1118 && e.kind != EsKind::Berserk &&
        (e.kind != EsKind::DeadAlive || e.adder_on))
        es_stat_update(e.stat);                            // (ESADeadAlive: only while active, sub_587D30)
    if (e.result == 1 && e.kind == EsKind::ScrollStatUp) es_scroll_update(e);   // sub_58E4F0
    if (e.kind == EsKind::SharpSense) power_shot_mult_ = 1.0f;
    if (e.kind == EsKind::SharpSenseEx) gun_skill_mult_ = 1.0f;   // sub_58B590
    if (e.kind == EsKind::HammerWheel) {                           // Hammer Spin over: our own speed back
        spin_speed_ = 0.0f;
        apply_move_type_speed(motion_.moving(std::chrono::steady_clock::now()) ? motion_.type : 2);
    }
    if (e.kind == EsKind::Mutation) {                              // sub_574D00 / sub_5BB570
        if (e.orphan) {
            // no caster ref (a restored one): HP = (int)(HP * HpRemainRatio), S2C 0x0024 (sub_5F4670)
            hp_ = static_cast<int32_t>(static_cast<double>(hp_) * static_cast<double>(e.ratio));
            PacketBuffer p(static_cast<Opcode>(0x0024), is_paula());
            p.write<uint32_t>(static_cast<uint32_t>(hp_));
            send(p);
        } else {
            if (e.esaction == 1494) {                              // sub_5FEC70: S2C 582 [u16 us][the look] first
                PacketBuffer p(static_cast<Opcode>(0x0246), is_paula());
                p.write<uint16_t>(static_cast<uint16_t>(object_id_));
                write_equip_appearance(p);
                server_.broadcast_zone(zone_id_, p, 0);
            }
            if (!mutation_end(e)) {                                // the end refused: the player is cut (sub_8AA290)
                LOG_ERROR("GameClientSession %u: the mutation's end failed — cut (retail)", session_id());
                close();
            }
        }
    }
    if (e.kind == EsKind::Bunker && pcond_test(1)) pcond_remove(1);   // sub_5BA970 (the adder off: below)
    if (e.kind == EsKind::FluentWaist && pcond_test(2048)) pcond_remove(2048);   // sub_560AC0
    if (e.kind == EsKind::Kamikaze && !e.orphan && pcond_test(8)) pcond_remove(8);   // sub_5B8A00 (its owner there)
    if (e.kind == EsKind::KamikazeDo && pcond_test(0x80000)) pcond_remove(0x80000);   // sub_5B8CA0
    if (e.kind == EsKind::GMCheer) gm_cheer_update();      // sub_596EF0: the adder out, the update
    if (e.kind == EsKind::OpticalEyes && !e.ids.empty()) {         // sub_574310: the seen ones hidden again
        PacketBuffer p(static_cast<Opcode>(0x033C), is_paula());
        p.write<uint8_t>(0);
        p.write<uint16_t>(static_cast<uint16_t>(e.ids.size()));
        for (uint16_t id : e.ids) p.write<uint16_t>(id);
        send(p);
    }
    if (e.kind == EsKind::Boiling) cdm_.boost = 0.0f;      // sub_58F040: sub_76A460(0.0)
    if (e.result == 1 && e.kind == EsKind::SkillResist && e.adder_on)                      // sub_580B00 ...
        for (int s : skill_resist_ids(e.esaction)) skill_resist_sub(s, e.value);
    if (e.kind == EsKind::Hiding && !e.orphan) hide_set(false);                            // sub_570F80
    if (e.kind == EsKind::Berserk && pcond_test(1256)) pcond_remove(1256);                 // sub_562590
    if (e.kind == EsKind::ElementUp) element_up_apply(e, false);   // sub_5B78C0
    if (e.kind == EsKind::Field) field_end_children(e);             // sub_53E6A0
    if (e.kind == EsKind::ElemArea) elem_area_end(e);               // sub_5899F0
    if (e.kind == EsKind::Bind && e.first) {                        // sub_534BF0 (+308): its ESABindDo, cond 10
        std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
        for (uint16_t id : e.ids) {
            MonsterEntity* m = server_.entity_mgr().get_monster(id);
            if (!m) continue;
            for (size_t j = 0; j < m->es.size(); ++j)
                if (m->es[j].esaction == 1155) { server_.monster_es_end(*m, j); break; }
        }
        if (pcond_test(10)) pcond_remove(10);
    }
    if (e.kind == EsKind::DarkBarrier && e.result == 1) elem_es_add(17, static_cast<int16_t>(-e.value));   // sub_56D610
    if (e.kind == EsKind::LightShield) {                            // sub_56CF80
        elem_es_add(16, static_cast<int16_t>(-e.value));
        es_light_shield_update();
    }
    if (e.kind == EsKind::BloodyOath && e.result == 1) bloody_on_ = false;   // sub_586480
    if (e.kind == EsKind::SettleDown) cd_ratio_ = 1.0f;                      // sub_5B9F20
    if (e.kind == EsKind::DarkUp) elem_es_add(6, static_cast<int16_t>(-e.value));   // sub_5BA330 -> sub_5BA1F0(0)
    if (e.kind == EsKind::ElemShield && e.result == 1) {   // sub_56B280: its blessing ends, the Value off
        es_remove_esaction(1337 + ((e.esaction == 1247) ? 0 : (e.esaction - 1249)));
        elem_es_add(e.count, static_cast<int16_t>(-e.value));
    }
    if (e.kind == EsKind::FlashCut) {                      // sub_5737D0: we stop, our speed back
        skill_stop_motion(std::chrono::steady_clock::now());
        set_current_speed(e.ratio);
    }
    LOG_DEBUG("GameClientSession %u: ES %u ESAction %d ended", session_id(), e.id, e.esaction);
}

// sub_56B190 and its siblings: Character +924 [slot] += v (sub_718280 recomputes +836), then S2C 826 [u8 1][u8 slot]
// [u16 +924 slot] to the player (sub_602E90). VERIFIED: the client's avatar (sub_6B4E00, opcode 826) stores each u16
// at +824 + 2 * slot and recomputes (sub_49D400).
void GameClientSession::elem_es_add(int slot, int16_t v)
{
    if (slot < 0 || slot >= 22) return;
    elem_es_[slot] = static_cast<int16_t>(elem_es_[slot] + v);
    PacketBuffer p(static_cast<Opcode>(0x033A), is_paula());
    p.write<uint8_t>(1);
    p.write<uint8_t>(static_cast<uint8_t>(slot));
    p.write<int16_t>(elem_es_[slot]);
    send(p);
}

// ESAElementUpDo sub_5B75D0: +924 [0..4] += / -= its five values (sub_718280), then S2C 826 [u8 5]{[u8 i][u16 +924 i]}.
void GameClientSession::element_up_apply(const ActiveEs& e, bool on)
{
    PacketBuffer p(static_cast<Opcode>(0x033A), is_paula());
    p.write<uint8_t>(5);
    for (int k = 0; k < 5; ++k) {
        elem_es_[k] = static_cast<int16_t>(elem_es_[k] + (on ? e.bonus[k] : -e.bonus[k]));
    }
    for (int k = 0; k < 5; ++k) { p.write<uint8_t>(static_cast<uint8_t>(k)); p.write<int16_t>(elem_es_[k]); }
    send(p);
}

// ESAEnergySword's part of our attack ranges (sub_588A40: +1044 / +1048 / +1052 += Range while it is on).
int GameClientSession::chi_sword_range() const
{
    int r = 0;
    for (const ActiveEs& e : es_) if (e.kind == EsKind::EnergySword && e.adder_on) r += e.value;
    return r;
}

// +0x148 flag 64 (ESAEnergySword on): our melee blows carry DamageInfo flag 0x400 (sub_5E5700).
bool GameClientSession::chi_sword_on() const
{
    for (const ActiveEs& e : es_) if (e.kind == EsKind::EnergySword && e.adder_on) return true;
    return false;
}

// The ESs' vtbl+8 on a damage event (30002) on us, hit or miss: ESASelfCureDo (sub_5B9780) ends. (ESAGuard's
// handler, sub_5B5E10, is done where the blow is rolled — mob_swing.)
void GameClientSession::es_on_damaged()
{
    for (size_t i = 0; i < es_.size();) {
        if (es_[i].kind == EsKind::SelfCure) { es_end(i); continue; }
        ++i;
    }
}

// sub_5D4610 on one of Suffocate's targets: the action {ESAction 1288, EState 0, ESClass 1, flags 2, skill 1503,
// the row} through sub_524690(.., 1) — 1503 is no active skill (Type 3): the monster's limiter admits it
// (ESClass 1: 5). ESAFluoresceneDo (InitESAction's roll on the row's ApplyRatio, then sub_571370): +196 = 1000 *
// Second (the row's +8), the lifetime +196, the block {S2C 224 sub_5EDE00 [us][target][ES id][1288][2][1][1503]
// [0], no tail — VERIFIED: the client's ESAFluoresceneDo 0x4F3D30 reads none}, the skill's TM per hit (a player
// caster, sub_891C80) whatever the roll; applied -> a hiding target (+452) loses its ESAHiding (1286; none on
// monsters) and takes the condition 0x20000 (sub_571500 — it forbids the skills whose flags have that bit,
// sub_891E70: no monster skills here). Its tick (sub_5714A0): +196, then the base tick (not applied -> the end);
// its end (sub_5715E0): the condition 0x20000 off when on.
void GameClientSession::es_fluorescene_do(MonsterEntity& m, const SkillDef& d, const EsaRow* act)
{
    std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
    SkillDef dd = d;
    dd.esaction = 1288;
    dd.estate   = 0;
    dd.esclass  = 1;
    if (!server_.monster_es_admit(m, dd)) return;
    MonsterEntity::Es e;
    e.id       = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
    e.esaction = 1288;
    e.estate   = 0;
    e.esclass  = 1;
    e.skill    = static_cast<uint16_t>(d.id);
    e.flags    = 2;
    e.caster   = object_id_;
    e.added    = tick_ms();
    e.result   = apply_roll(act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f, 0);
    e.end_ms   = 1000 * static_cast<int16_t>(act ? static_cast<int>(act->get("Second", 0.0)) : 0);
    e.life_ms  = e.end_ms;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(1288);
        o.write<uint8_t>(kEsKind);
        o.write<uint8_t>(1);
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(0);
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    m.es.push_back(e);
    skill_hit_tm(m, e.skill);                                // sub_891C80
    if (e.result == 1) {
        for (size_t i = 0; i < m.es.size(); ++i)
            if (m.es[i].esaction == 1286) { server_.monster_es_end(m, i); break; }
        m.cond_add(0x20000);
    }
    LOG_DEBUG("GameClientSession %u: Suffocate ES %u on mob %u result %d (%d ms)", session_id(), e.id,
              static_cast<unsigned>(m.object_id), e.result, e.end_ms);
}

// A dash ES's run (ESADash sub_55C320, ESAFlashAttacking sub_573600): MoveTo (x, y) with our move bits (+92) as
// the type — the move strategy takes that type's speed (S2C 0x0018) —, then the current speed `speed` (vtbl+92:
// S2C 0x01D8).
void GameClientSession::es_run_to(int16_t px, int16_t py, float speed, std::chrono::steady_clock::time_point now)
{
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    const uint8_t mt = motion_.moving(now) ? static_cast<uint8_t>(motion_.type) : 0;
    const uint8_t mtype = ((mt & 7) == 0) ? 2 : static_cast<uint8_t>(mt & 7);
    apply_move_type_speed(mtype);
    broadcast_move(static_cast<uint16_t>(object_id_), cx, cy, px, py, mt);
    set_current_speed(speed);
    motion_.start(cx, cy, px, py, cur_speed_, now, mtype);
    pos_x_ = px; pos_y_ = py;
    if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = px; p->pos_y = py; }
}

// ── A zone change ────────────────────────────────────────────────────────────────────────
//  Client::PrepareLeavingZone (sub_40B800) writes the ESLimiter (vtbl+100 sub_5E2210, with the
//  destination map's type): every list's ESs (ESs outside the limiter — ESClass -1, Type 2 skills —
//  are left behind) except ESAction 1492 and, into a map of type 1 / 5 / 6, an ES with the flag 16;
//  each one's entry (sub_5E33F0: list, ESClass, ESAction, its add time +16, ...) and its class's save
//  (vtbl+104: the base sub_50FC10 — flags, skill, lifetime +188 — then the class's fields).
//  The next zone's LoadESLimiterAll (sub_40E300 -> vtbl+28 sub_5E2360) makes each again under a NEW
//  object id (sub_7FD5C0 -> sub_50FAE0: no S2C 224, the 10 s warning again) with the base restore
//  sub_50FEB0 — applied (+180 = 1) whatever it was — and the class's restore (vtbl+120): the
//  StatusValueAdder joins the chain again and the stat is updated (recompute + S2C 283; not
//  ESAStaticShield), ESASharpSense sets +1064 again, ESABoiling the drill boost, ESAThirdEye posts the
//  event 30280. ESAction::LoadAll (sub_50FC70) refuses an ES whose skill is gone or whose SkillOption
//  forbids the new map's type (2: types 1 / 5; 512: type 1; 2048: types 3 / 10).
//  The client keeps its avatar across the move (its buff stats too); the old ES objects' ids mean
//  nothing to it afterwards (an S2C 280 of a new id finds nothing).
void cGameServer::es_put(uint32_t char_uid, std::vector<EsCarry> v)
{
    std::lock_guard<std::mutex> lock(es_carry_mutex_);
    EsCarryList& l = es_carry_[char_uid];
    l.v  = std::move(v);
    l.at = std::chrono::steady_clock::now();
}

bool cGameServer::es_take(uint32_t char_uid, std::vector<EsCarry>& out)
{
    std::lock_guard<std::mutex> lock(es_carry_mutex_);
    auto it = es_carry_.find(char_uid);
    if (it == es_carry_.end()) return false;
    EsCarryList l = std::move(it->second);
    es_carry_.erase(it);
    if (std::chrono::steady_clock::now() - l.at > std::chrono::seconds(60)) return false;
    out = std::move(l.v);
    return true;
}

void GameClientSession::es_carry_out(int zone_to)
{
    const MapDef* z = server_.maps_db().get(zone_to);
    const int type = z ? z->type : 0;
    std::vector<cGameServer::EsCarry> v;
    for (const ActiveEs& e : es_) {
        if (!e.limited) continue;
        if ((type == 1 || type == 5 || type == 6) && (e.flags & 16)) continue;
        if (e.esaction == 1492) continue;
        cGameServer::EsCarry c;
        c.esaction   = e.esaction;
        c.estate     = e.estate;
        c.esclass    = e.esclass;
        c.skill      = e.skill;
        c.level      = e.level;
        c.flags      = e.flags;
        c.life_ms    = e.life_ms;
        c.elapsed_ms = e.elapsed_ms;
        c.end_ms     = e.end_ms;
        c.added      = e.added;
        c.kind       = static_cast<uint8_t>(e.kind);
        c.stat       = e.stat;
        c.ratio      = e.ratio;
        c.value      = e.value;
        c.countdown  = e.countdown;
        for (int k = 0; k < 12; ++k) c.bonus[k] = e.bonus[k];
        c.child      = e.child;
        c.radius     = e.radius;
        c.bolster    = e.bolster;
        c.count      = e.count;
        c.base_ratio = e.base_ratio;
        c.adder_on   = e.adder_on;
        c.threshold  = e.threshold;
        c.hp_ratio   = e.hp_ratio;
        c.last_ms    = e.last_ms;
        c.hp_drop    = e.hp_drop;
        c.mp_drop    = e.mp_drop;
        c.interval_ms  = e.interval_ms;                    // ESAShield's save sub_527420 (+236 / +240), ESASaintPromise's
        c.interval_acc = e.interval_acc;                   //   sub_585EE0 (+204). (ESASelfCureDo's save sub_5B98C0 never runs:
                                                           //   Chakra Balance makes it outside the limiter, so it stays behind.)
        v.push_back(c);
    }
    LOG_DEBUG("GameClientSession %u: %zu effect states go along to zone %d", session_id(), v.size(), zone_to);
    server_.es_put(char_uid_, std::move(v));
}

void GameClientSession::es_restore_carry()
{
    std::vector<cGameServer::EsCarry> v;
    if (!server_.es_take(char_uid_, v)) return;
    const MapDef* z = server_.maps_db().get(zone_id_);
    const int type = z ? z->type : 0;
    size_t n = 0;
    for (const cGameServer::EsCarry& c : v) {
        const SkillDef* sd = server_.skill_db().get(c.skill);     // sub_50FC70
        if (!sd) {
            LOG_ERROR("ESAction::LoadAll() - not found skill or don't set m_TriggerId, m_TriggerId=%u", c.skill);
            continue;
        }
        if (((sd->skill_option & 2) && (type == 1 || type == 5)) || ((sd->skill_option & 512) && type == 1) ||
            ((sd->skill_option & 2048) && (type == 3 || type == 10)))
            continue;
        ActiveEs e;
        e.id         = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
        e.esaction   = c.esaction;
        e.estate     = c.estate;
        e.esclass    = c.esclass;
        e.skill      = c.skill;
        e.level      = c.level;
        e.flags      = c.flags;
        e.result     = 1;                                  // sub_50FEB0: +180 = 1
        e.warned     = false;                              // sub_50FAE0: +185 = 0
        e.limited    = true;
        e.life_ms    = c.life_ms;
        e.elapsed_ms = c.elapsed_ms;
        e.end_ms     = c.end_ms;
        e.added      = c.added;
        e.kind       = static_cast<EsKind>(c.kind);
        e.stat       = c.stat;
        e.ratio      = c.ratio;
        e.value      = c.value;
        e.countdown  = c.countdown;
        for (int k = 0; k < 12; ++k) e.bonus[k] = c.bonus[k];
        e.child      = c.child;
        e.radius     = c.radius;
        e.bolster    = c.bolster;
        e.count      = c.count;
        e.base_ratio = c.base_ratio;
        e.interval_ms  = c.interval_ms;                    // ESAShield's restore sub_5274B0 (+236 / +240), ESASaintPromise's
        e.interval_acc = c.interval_acc;                   //   sub_585F30 (+204). (ESASelfCureDo's restore sub_5B9840 never
                                                           //   runs: an ESASelfCureDo is never carried, see es_carry_out.)
        if (e.kind == EsKind::DeadAlive) {                            // sub_587F20
            e.adder_on  = c.adder_on;
            e.threshold = c.threshold;
            e.hp_ratio  = c.hp_ratio;
            e.last_ms   = c.last_ms;
        }
        if (e.kind == EsKind::Berserk) {                              // sub_561B10: the row; the condition again
            e.hp_drop = c.hp_drop;
            e.mp_drop = c.mp_drop;
            pcond_add(1256);
        }
        if (e.kind == EsKind::FluentWaist) {                          // sub_560A80 -> sub_560A00: we stop, 2048
            skill_stop_motion(std::chrono::steady_clock::now());
            pcond_add(2048);
        }
        if (e.kind == EsKind::ScrollShell) e.elapsed_ms = e.end_ms;   // sub_58D940: +196 = 0, it ends next tick
        if (e.kind == EsKind::Anniversary) e.adder_on = true;         // sub_576320: into the chain at once
        if (e.kind == EsKind::Aura)                                   // sub_555AD0 / sub_5568C0 / sub_557A80: a
            e.pulse_ms = (e.esaction == 1210 || e.esaction == 1212 || e.esaction == 1175) ? 0 : 400;   //   pulse at
                                                                           //   once; ESAHawkEye / ESAMultiAgilityUp /
                                                                           //   ESAGoodLuck (sub_5638D0) 0
        if (e.kind == EsKind::Hiding) e.orphan = true;                // sub_571120: no player ref -> it ends
        if (e.kind == EsKind::Kamikaze) e.orphan = true;              // sub_5B8AB0: no owner ref -> it ends (no boom)
        if (e.kind == EsKind::Mutation || e.kind == EsKind::SharpClaw)   // sub_574FC0 / sub_5BB630 / sub_56A060: no
            e.orphan = true;                                          //   refs -> it ends on its next tick
        if (e.kind == EsKind::OpticalEyes) e.pulse_ms = 400;          // sub_5744C0: a look-around at once (its seen
                                                                      //   list is not carried here)
        if (e.kind == EsKind::AuraChild) {                            // sub_556200: inactive, no caster -> it
            e.adder_on = false;                                       //   ends on its next tick
            e.orphan   = true;
        }
        es_.push_back(e);
        ++n;
        // the class's restore (vtbl+120)
        if (e.stat >= 0 && e.esaction != 1118 && e.kind != EsKind::AuraChild && e.kind != EsKind::Berserk &&
            (e.kind != EsKind::DeadAlive || e.adder_on))
            es_stat_update(e.stat);
        if (e.kind == EsKind::ScrollStatUp) es_scroll_update(e);       // sub_58E680
        if (e.kind == EsKind::SharpSense) power_shot_mult_ = e.ratio;   // sub_568CF0
        if (e.kind == EsKind::SharpSenseEx) gun_skill_mult_ = e.ratio;  // sub_58B610
        if (e.kind == EsKind::Bunker)                                   // sub_5BAA00: the adder only with shield
            es_.back().adder_on = (e.value > 0);                        //   left (no condition 1 again: retail)
        if (e.kind == EsKind::Boiling) cdm_.boost = e.ratio;            // sub_76A460
        if (e.kind == EsKind::ThirdEye) third_eye_on();                 // event 30280
        if (e.kind == EsKind::SkillResist) {                            // sub_580BB0 / sub_56EB20 / ...: only
            es_.back().adder_on = (e.esaction == 1270);                 //   ESAInsulatorDo finds its target again
            if (e.esaction == 1270)
                for (int s : skill_resist_ids(e.esaction)) skill_resist_add(s, e.value);
        }
        if (e.kind == EsKind::ElemShield) elem_es_add(e.count, static_cast<int16_t>(e.value));   // sub_56B370
        if (e.kind == EsKind::ElementUp) element_up_apply(e, true);                               // sub_5B7910
        if (e.kind == EsKind::GMCheer) gm_cheer_update();                                         // sub_5970B0
        if (e.kind == EsKind::LightShield) {                            // sub_56D0E0: the adder again, LightR
            elem_es_add(16, static_cast<int16_t>(e.value));             //   (sub_56CD90(1)), the recompute + S2C 283
            es_light_shield_update();                                   //   (sub_56CE40)
        }
        if (e.kind == EsKind::DarkBarrier) {                            // sub_56D6E0: DarkR (sub_56D540(1)); no blow
            es_.back().interval_acc = 0;                                //   pending (+224 / +228 = 0)
            elem_es_add(17, static_cast<int16_t>(e.value));
        }
        if (e.kind == EsKind::DarkUp) elem_es_add(6, static_cast<int16_t>(e.value));             // sub_5BA3F0 -> sub_5BA1F0(1)
        // (ESABloodyOath's restore sub_586520 and ESASettleDownDo's sub_5B9F80 set nothing on the player: Blood
        //  Testament's +1616.. and Refreshing Dew's cool-time speed stay off after a zone change — retail.)
    }
    if (n) recompute_stats(true);
    LOG_DEBUG("GameClientSession %u: %zu of %zu effect states made again in zone %d", session_id(), n, v.size(), zone_id_);
}

// The player leaves the world: its ESs go with it (a zone change carries the limiter's first:
// es_carry_out).
void GameClientSession::es_end_all()
{
    for (const ActiveEs& e : es_) server_.entity_mgr().free_oid(e.id);
    es_.clear();
    skill_resist_.clear();
    for (bool& u : pcond_used_) u = false;                 // (the conditions the ESs had set)
    for (int32_t& b : buff_) b = 0;
    for (int16_t& v : elem_es_) v = 0;                    // (their element values)
    power_shot_mult_ = 1.0f;
    gun_skill_mult_  = 1.0f;
    cdm_.boost = 0.0f;
}

// Player::OnDie (sub_73BFA0): the ESLimiter (+2340) clears its lists 0, 1 and 7 (vtbl+4 sub_5E1830 ->
// sub_5E3080: each entry's ES ends through its vtbl+28 — S2C 280 — sub_5E3010(.., 0)).
void GameClientSession::es_end_death_lists()
{
    for (size_t i = 0; i < es_.size();) {
        const int list = es_list(es_[i].esclass);
        if (es_[i].limited && (list == 0 || list == 1 || list == 7)) { es_end(i); continue; }
        ++i;
    }
}

// Character::OnMessage (sub_715A90) event 30280: the Third Eye goes on (+1628, sub_719920).
void GameClientSession::third_eye_on()
{
    if (!third_eye_) third_eye_ = true;
}

// Event 30281: the Third Eye goes off; a shadow monster we are attacking is dropped as our target
// (sub_41C670(+232) -> sub_41EB50: the attack strategy's target ref is cleared — the follow ends).
void GameClientSession::third_eye_off()
{
    if (!third_eye_) return;
    third_eye_ = false;
    if (attacking_) {
        const MonsterEntity* m = server_.entity_mgr().get_monster(atk_target_);
        if (m && m->shadow()) end_attack(true);
    }
}

// PlayerESLimiter vtbl+60 (sub_5E28A0): the first ES of that ESAction in the lists ends.
bool GameClientSession::es_remove_esaction(int esaction)
{
    for (size_t i = 0; i < es_.size(); ++i) {
        if (es_[i].limited && es_[i].esaction == esaction) {
            es_end(i);                                     // sub_5E3010(v3, 0)
            return true;
        }
    }
    return false;
}

// PlayerESLimiter vtbl+84 (sub_5E2740): the limiter's ES of that ESAction (vtbl+80 sub_5E26C0 =
// whether there is one).
const GameClientSession::ActiveEs* GameClientSession::es_find(int esaction) const
{
    for (const ActiveEs& e : es_)
        if (e.limited && e.esaction == esaction) return &e;
    return nullptr;
}

// ResChain 9 (sub_716820): stat i's buff = the StatusValueAdder chain, in registration order, on
// (float)(base + items) from 0.0 — each element adds its term to the running float — then (int).
int32_t GameClientSession::es_stat_bonus(int stat, float base_item) const
{
    float acc = 0.0f;
    // The learned passives' ESs come first: entering the world (sub_760E30), learning (sub_6E59C0 /
    // sub_6E7460) and a level-up (sub_780FD0) put each Type-1 skill with no PSAbility into the PS
    // limiter (+4504, sub_525220) as an ES of its ESAction, built from the learned level's row and
    // kept while learned — no packet (the client counts them itself). Their adders: 1084
    // ESAManaBottle MaxMP += MP, 1091 ESABigBag1 MaxWT += WT, 1169 ESAFitness MaxHP += HP (the row's
    // int +4; sub_54B220 / sub_54B5F0 / sub_560E00). (SkillOption 512 passives are off in zone types
    // 1/5/6 — none of these.)
    for (const LearnedSkill& ls : skills_) {
        const SkillDef* sd = server_.skill_db().get(ls.id);
        if (!sd || sd->type != 1 || sd->ps_ability) continue;
        int pstat = -1;
        const char* col = nullptr;
        switch (sd->esaction) {
        case 1084: pstat = 3; col = "MP"; break;
        case 1091: pstat = 6; col = "WT"; break;
        case 1169: pstat = 9; col = "HP"; break;
        default: break;
        }
        if (pstat != stat) continue;
        const EsaRow* r = server_.esa_db().row(sd->esa_table, ls.level);
        if (r) acc = static_cast<float>(static_cast<double>(static_cast<int32_t>(r->get(col, 0.0))) + acc);
    }
    for (const ActiveEs& e : es_) {
        if (e.kind == EsKind::LightShield && (stat == 5 || stat == 10)) {   // sub_56CEC0 (in the chain whatever the
            const float v = static_cast<float>(static_cast<double>(16 * (stat_[4] - 49)) *     //   roll)
                                               static_cast<double>(stat == 5 ? e.base_ratio : e.ratio));
            if (v > 0.0f) acc = acc + v;
            continue;
        }
        if (e.result != 1) continue;
        if (e.kind == EsKind::ScrollStatUp) {               // sub_58E7F0: every stat, its bonus (0 for most)
            acc = static_cast<float>(static_cast<double>(e.bonus[stat]) + acc);
            continue;
        }
        if (e.kind == EsKind::GMCheer) {                    // sub_597310: AP AC MA MD DA LK DP HV — flat, or (20002 /
            if (stat == 0 || stat == 1 || stat == 4 || stat == 5 || stat == 7 || stat == 8 || stat == 10 || stat == 11) {
                const int32_t v = static_cast<int16_t>(e.bonus[stat]);       //   20003) base (+704) * value / 100
                if (e.value) acc = static_cast<float>(static_cast<double>(base_stat(stat) * v / 100) + acc);
                else         acc = static_cast<float>(static_cast<double>(v) + acc);
            }
            continue;
        }
        if (e.stat != stat) continue;
        switch (e.kind) {
        case EsKind::StatRatio:   acc = static_cast<float>(static_cast<double>(base_item) * e.ratio + acc); break;
        case EsKind::BearStat:    acc = static_cast<float>(static_cast<double>(base_item) * e.ratio +
                                                           static_cast<double>(e.value) + acc);       break;
        case EsKind::StatValue:   acc = static_cast<float>(static_cast<double>(e.value) + acc);             break;
        case EsKind::PriMarksman: acc = static_cast<float>(static_cast<double>(e.value) / 20.0 + acc);      break;
        case EsKind::Anniversary:                          // sub_576450: once in the chain
            if (e.adder_on) acc = static_cast<float>(static_cast<double>(base_item) * e.ratio + acc);
            break;
        case EsKind::AuraChild:                            // 1126 AP (sub_5562D0) / 1130 DP (sub_5583D0) / 1211 AC
            if (e.esaction == 1213) acc = static_cast<float>(static_cast<double>(e.value) + acc);   // DX (sub_5679B0)
            else acc = static_cast<float>(static_cast<double>(base_item) * e.ratio + acc);
            break;
        case EsKind::Bunker:                               // sub_5BAC40: DP += +224 (in the chain with shield left)
            if (e.adder_on) acc = static_cast<float>(static_cast<double>(e.bonus[10]) + acc);
            break;
        case EsKind::Berserk:                              // sub_5623F0: AP += base * ApRatio - base (base > 0)
            if (base_item > 0.0f) acc = base_item * e.ratio - base_item + acc;
            break;
        case EsKind::DeadAlive:                            // sub_5881C0: while active
            if (e.adder_on) {
                const float v = static_cast<float>(static_cast<double>(base_item) *
                                                   (static_cast<double>(e.ratio) - e.hp_ratio * 2.5));
                acc = v + acc;
            }
            break;
        default: break;
        }
    }
    return static_cast<int32_t>(acc);
}

// ESALightShield's update (sub_56CE40): the recompute, then S2C 283 [u8 2][u8 5][u32 MD buff (+676)][u8 10][u32 DP
// buff (+696)] to us.
void GameClientSession::es_light_shield_update()
{
    recompute_stats(true);
    PacketBuffer p(static_cast<Opcode>(0x011B), is_paula());
    p.write<uint8_t>(2);
    p.write<uint8_t>(5);
    p.write<uint32_t>(static_cast<uint32_t>(buff_[5]));
    p.write<uint8_t>(10);
    p.write<uint32_t>(static_cast<uint32_t>(buff_[10]));
    send(p);
}

// The class's update (e.g. sub_54C160): the target's vtbl+160 (recompute, HP/MP clamped) and
// S2C 283 [u8 1][u8 stat][u32 the stat's buff] to it.
void GameClientSession::es_stat_update(int stat)
{
    recompute_stats(true);
    if (stat < 0 || stat >= 12) return;
    PacketBuffer p(static_cast<Opcode>(0x011B), is_paula());
    p.write<uint8_t>(1);
    p.write<uint8_t>(static_cast<uint8_t>(stat));
    p.write<uint32_t>(static_cast<uint32_t>(buff_[stat]));
    send(p);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Effect states on monsters (the ESActions sub_524950 sends to the selected object, sub_524F60).
//
//  The monster's MonsterESLimiter (CommonMonster +1856, sub_72BE90): ESClass lists 10,5,5,0,1,0,0,0,
//  the same admission (sub_5E1CE0). While an ES is up the monster's stats are base (+704) + the
//  ResChain 9 sum (CommonMonster vtbl+160 = sub_716680 kind 3, sub_716820 on (float)base).
//  The classes done (their vtbl+116 / +56 / +100 and adders; the client's class reads [u8]):
//    1109 ESASmoothFloor (Sticky Foot)      HV -= caster DA * Ratio / 2.5 (if > 0)    sub_551D30
//    1110 ESAShieldBreak (Armor Destructor) DP -= the monster's DP * Ratio (if > 0)   sub_552560
//    1363 ESALockOn (Lock-on)               HV -= caster AC * Ratio / 2.5             sub_5877F0
//    1081 ESADpDown (Armor Breaker)         DP -= caster AP * Ratio                    sub_54AB00
//    1098 ESAAcDown (Shock Vibe)            AC -= caster AP * Ratio / 16 + 49          sub_54E870
//    1099 ESASenceDown (Sense Breaker)      DA -= caster AP * Ratio / 16 (if > 0)     sub_54F040
//    1102 ESALuckDown (Luck Breaker)        LK -= caster AP * Ratio / 16 (if > 0)     sub_54FF70
//    1106 ESAMagicWeb (Mana Web)            DX += caster MA * Ratio / 100 (if > 0)    sub_550D70
//    1107 ESADarkFog (Rust)                 AP -= 16 * (caster MA - 49) * Ratio (> 0) sub_551550
//      (ESADpDown's tail is [u16 the target][u8 result] — sub_54A550 / client ESADpDown)
//      (sub_551810 / sub_552040 / sub_5872E0: Ratio, Seconds; the caster (+220) and sub_5E6D70 —
//      the caster may attack it; the caster marks it (vtbl+132); applied -> recompute + the
//      caster's TM per hit sub_891C80. Tick: past Seconds, or the caster gone -> end; end: the
//      chain out + recompute, and the caster's vtbl+132(target, 5000).)
//    1324 ESABreakBarrier (Shield Breaker) — sub_5800F0: a player caster needs a gun (sub_6DA700(54))
//      and a loaded magazine, else S2C 239 [18] and no object; Ratio, 1000 * Seconds ms; applied ->
//      cut = min(80, (int)((the monster's DA * Ratio + 20.0) / 3.0)) off its PhysicalR and GunR
//      (sub_580490, +964 / +962), its barrier ESs 1118 / 1130 / 1129 / 1311 end (sub_5803C0), one
//      bullet (sub_894560); end: the cut goes back.
//  A boss's DebuffSet against the skill's DebuffBitCode (sub_891F80 -> S2C 1247 instead of the ES):
//  monster_debuff_immune / send_debuff_resisted (skill.cpp); normal monsters have no DebuffSet.
//  (The classes above are only some of those done. es_apply_on's first check lists the ESActions it takes;
//  three are made outside it: es_fluorescene_do (1288), es_electric_stun (1471) and quake_paralysis (1531,
//  skill_bear.cpp). Grep "MonsterEntity::Es e;" for every place that makes one.)
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// MonsterESLimiter (sub_72BE90 -> sub_5E16A0(10, 5, 5, 0, 1, 0) -> sub_5E1320).
constexpr int kMonsterEsCapacity[kEsLists] = { 10, 5, 5, 0, 1, 0, 0, 0 };

bool monster_es_needs_caster(int esaction)
{
    switch (esaction) {
    case 1081: case 1096: case 1098: case 1099: case 1102: case 1106: case 1107: case 1109: case 1110: case 1117:
    case 1363: case 1189: case 1320: case 1243:
    case 1543:                                             // ESABearAttack (skill_bear.cpp): the caster's AP
        return true;
    default: return false;
    }
}
} // namespace

// The limiter's admission (sub_5E1CE0, the non-debuff branch) on the monster's lists.
bool cGameServer::monster_es_admit(MonsterEntity& m, const SkillDef& d)
{
    const int list = es_list(d.esclass);
    if (list < 0 || list >= kEsLists) return false;
    if (es_debuff_class(d.esclass)) {
        LOG_DEBUG("cGameServer: skill %d debuff class %d on a monster — not implemented", d.id, d.esclass);
        return false;
    }
    for (size_t i = 0; i < m.es.size(); ++i) {
        if (es_list(m.es[i].esclass) == list && m.es[i].esaction == d.esaction) {
            monster_es_end(m, i);
            return true;
        }
    }
    int used = 0;
    size_t oldest = m.es.size();
    for (size_t i = 0; i < m.es.size(); ++i) {
        if (es_list(m.es[i].esclass) != list) continue;
        ++used;
        if (oldest == m.es.size() || m.es[i].added < m.es[oldest].added) oldest = i;
    }
    if (used < kMonsterEsCapacity[list]) return true;
    if (oldest == m.es.size()) return false;
    monster_es_end(m, oldest);                               // (no notice: the target is no player)
    return true;
}

// CommonMonster vtbl+160 (sub_716680, kind 3): each stat = base + (int)(the chain on (float)base),
// the adders in registration order; ShieldBreak reads the monster's DP of the previous recompute.
void cGameServer::monster_recompute(MonsterEntity& m)
{
    if (!m.base_set) return;
    struct S { int stat; int32_t base; int32_t* out; };
    S stats[] = { { 0, m.base_ap, &m.ap }, { 1, m.base_ac, &m.ac }, { 2, m.base_dx, &m.dx }, { 4, m.base_ma, &m.ma },
                  { 5, m.base_md, &m.md }, { 7, m.base_da, &m.da }, { 8, m.base_lk, &m.lk }, { 10, m.base_dp, &m.dp },
                  { 11, m.base_hv, &m.hv } };
    for (S& s : stats) {
        float acc = 0.0f;
        for (const MonsterEntity::Es& e : m.es) {
            if (e.result != 1) continue;
            if (e.esaction == 1453 || e.esaction == 1456) {       // ESAGMCheer*Do sub_597310 (GM's Support: flat)
                if (s.stat == 0 || s.stat == 1 || s.stat == 4 || s.stat == 5 || s.stat == 7 || s.stat == 8 ||
                    s.stat == 10 || s.stat == 11)
                    acc = static_cast<float>(static_cast<double>(e.bonus[s.stat]) + acc);
                continue;
            }
            GameClientSession* c = monster_es_needs_caster(e.esaction) ? player_session(e.caster) : nullptr;
            if (monster_es_needs_caster(e.esaction) && !c) continue;
            if ((e.esaction == 1081 || e.esaction == 1543) && s.stat == 10) {   // (1543 ESABearAttack: skill_bear.cpp)
                acc = static_cast<float>(static_cast<double>(acc) - static_cast<double>(c->stat_value(0)) * e.ratio);
            } else if (e.esaction == 1537 && s.stat == 10) {  // ESABbongHammer (skill_bear.cpp): DP -= its DP * DPRatio
                const float v = static_cast<float>(static_cast<double>(s.base) * static_cast<double>(e.ratio3));
                if (v > 0.0f) acc = acc - v;
            } else if (e.esaction == 1098 && s.stat == 1) {
                acc = static_cast<float>(static_cast<double>(acc) -
                                         (static_cast<double>(c->stat_value(0)) * e.ratio / 16.0 + 49.0));
            } else if ((e.esaction == 1099 && s.stat == 7) || (e.esaction == 1102 && s.stat == 8)) {
                const float v = static_cast<float>(static_cast<double>(c->stat_value(0)) * e.ratio / 16.0);
                if (v > 0.0f) acc = acc - v;
            } else if (e.esaction == 1320 && s.stat == 2) {   // ESAHarden sub_57F510: the chain's value BECOMES
                acc = static_cast<float>(std::sqrt(static_cast<float>(s.base)) + static_cast<float>(s.base));   //   sqrtf(base) + base
            } else if (e.esaction == 1189 && s.stat == 2) {   // ESAWaterNetDo sub_5650C0: DX += caster MA * Ratio / 100
                acc = static_cast<float>(static_cast<double>(c->stat_value(4)) * e.ratio / 100.0 + acc);
            } else if (e.esaction == 1106 && s.stat == 2) {
                const float v = static_cast<float>(static_cast<double>(c->stat_value(4)) * e.ratio / 100.0);
                if (v > 0.0f) acc = acc + v;
            } else if (e.esaction == 1107 && s.stat == 0) {
                const float v = static_cast<float>(static_cast<double>(16 * (c->stat_value(4) - 49)) * e.ratio);
                if (v > 0.0f) acc = acc - v;
            } else if (e.esaction == 1110 && s.stat == 10) {
                const float v = static_cast<float>(static_cast<double>(*s.out) * e.ratio);
                if (v > 0.0f) acc = acc - v;
            } else if (e.esaction == 1109 && s.stat == 11) {
                const float v = static_cast<float>(static_cast<double>(c->stat_value(7)) * e.ratio / 2.5);
                if (v > 0.0f) acc = acc - v;
            } else if (e.esaction == 1363 && s.stat == 11) {
                const float v = static_cast<float>(static_cast<double>(c->stat_value(1)) * e.ratio / 2.5);
                acc = acc - v;
            } else if (e.esaction == 1096 && s.stat == 4) {   // ESASleepVoice sub_54DA40: MA -= caster HV * Ratio
                const float v = static_cast<float>(static_cast<double>(c->stat_value(11)) * e.ratio);
                if (v > 0.0f) acc = acc - v;
            } else if (e.esaction == 1117 && s.stat == 5) {   // ESAMDDown sub_554930: MD -= caster HV * Ratio * 16
                const float v = static_cast<float>(static_cast<double>(c->stat_value(11)) * e.ratio * 16.0);
                if (v > 0.0f) acc = acc - v;
            } else if (e.esaction == 1243) {
                // ESAAdhesiveDo sub_56AC20 (Sticky Icky), each part unless the DebuffSet resisted its bit (+192): HV
                // -= caster LK * HvDRatio * 0.4 on a monster (0x20000), LK -= caster LK * LkDRatio — at least 1 —
                // (0x8000), DP -= 16 * caster LK * DpDRatio (0x10000).
                const double lk = static_cast<double>(c->stat_value(8));
                if (s.stat == 11 && !(e.resisted & 0x20000)) {
                    const double v = lk * static_cast<double>(e.ratio) * 0.4000000059604645;
                    if (v > 0.0) acc = static_cast<float>(static_cast<double>(acc) - v);
                } else if (s.stat == 8 && !(e.resisted & 0x8000)) {
                    double v = lk * static_cast<double>(e.ratio2);
                    if (v <= 0.0) v = 1.0;
                    acc = static_cast<float>(static_cast<double>(acc) - v);
                } else if (s.stat == 10 && !(e.resisted & 0x10000)) {
                    const double v = static_cast<double>(16 * c->stat_value(8)) * static_cast<double>(e.ratio3);
                    if (v > 0.0) acc = static_cast<float>(static_cast<double>(acc) - v);
                }
            }
        }
        *s.out = s.base + static_cast<int32_t>(acc);
    }
}

// A caster's vtbl+132(target, 5000) at an ES's end: it marks the monster when it has none (sub_72B2F0).
void cGameServer::mark_owner_of(MonsterEntity& m, uint32_t caster)
{
    if (m.owner_oid) return;
    m.owner_oid = caster;
    m.owner_hp  = m.hp;
    PacketBuffer p(static_cast<Opcode>(0x016D), janus::PaulaCrypt::Instance().IsLoaded());
    p.write<uint16_t>(static_cast<uint16_t>(m.object_id));
    p.write<uint16_t>(static_cast<uint16_t>(caster));
    broadcast_zone(m.zone, p, 0);
}

// CommonMonster vtbl+120 (sub_72B400): an owned one lets its owner (+1212) go — S2C 366 [u16 it] to its area
// (sub_5EE620; VERIFIED: the client's object handler sub_49DCE0 takes 366 and drops its owner mark, sub_49D7C0),
// its +92 bit 8 off, the owner cleared.
void cGameServer::release_owner(MonsterEntity& m)
{
    if (!m.owner_oid) return;
    if (!m.banished) {                                       // (to its layer: none sees a banished one)
        PacketBuffer p(static_cast<Opcode>(0x016E), janus::PaulaCrypt::Instance().IsLoaded());
        p.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        broadcast_zone(m.zone, p, 0);
    }
    m.owner_oid = 0;
    m.owner_hp  = 0;
}

// sub_717DB0 on a player (Player::OnDie sub_73BFA0, ESADimensionDo's init sub_5B99A0): its back-references
// (Character +232, sub_5E9030) — each monster whose owner (+1212) is it lets it go (vtbl+120).
void cGameServer::release_owned(uint32_t player)
{
    for (uint32_t oid : entity_mgr_.monsters_owned_by(player))
        if (MonsterEntity* m = entity_mgr_.get_monster(oid)) release_owner(*m);
}

// vtbl+28 (sub_50F770) on a monster's ES: S2C 280 to the area, the class's vtbl+100.
void cGameServer::monster_es_end(MonsterEntity& m, size_t i)
{
    if (i >= m.es.size()) return;
    const MonsterEntity::Es e = m.es[i];
    m.es.erase(m.es.begin() + static_cast<std::ptrdiff_t>(i));
    entity_mgr_.free_oid(e.id);
    if ((e.flags & 2) && !(e.flags & 4) && !m.banished) {    // (to its layer: none sees a banished one)
        PacketBuffer p(static_cast<Opcode>(0x0118), janus::PaulaCrypt::Instance().IsLoaded());
        p.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        p.write<uint16_t>(e.id);
        broadcast_zone(m.zone, p, 0);
    }
    if (e.esaction >= 1370 && e.esaction <= 1378 && e.esaction % 2 == 0) {
        // ESA*AreaDo's release (vtbl+36 sub_58A7D0): applied -> the cut back (sub_58A650(1))
        const int k = e.phase - 11;
        if (e.result == 1 && k >= 0 && k < 8) m.resist_buff[k] = static_cast<int16_t>(m.resist_buff[k] + e.value);
    } else if (e.esaction == 1155) {
        // ESABindDo's end (sub_55B160): the condition 15 off; then sub_717DB0 on its caster (not the target): every
        // monster it owns lets it go (retail)
        if (m.cond_test(15)) m.cond_remove(15);
        if (e.caster && e.caster != m.object_id && player_session(e.caster)) release_owned(e.caster);
    } else if (e.esaction == 1322 && e.result == 1) {
        // ESADarkSlow's end (sub_57FA20 -> sub_57FBC0(1)): the speeds back
        const double v = static_cast<double>(e.value) / 100.0;
        m.walk_speed  = m.walk_speed  + static_cast<float>(v * 100.0);
        m.run_speed   = m.run_speed   + static_cast<float>(v * 160.0);
        m.chase_speed = m.chase_speed + static_cast<float>(v * 130.0);
    } else if (e.esaction == 1273) {
        if (m.cond_test(0x2000)) m.cond_remove(0x2000);      // ESAWinkOfCharm's end (sub_56F000)
    } else if (e.esaction == 1332) {
        if (m.cond_test(0xC0000)) m.cond_remove(0xC0000);    // sub_526E00
    } else if (e.esaction == 1279) {
        if (m.cond_test(4098)) m.cond_remove(4098);          // sub_56F440
    } else if (e.esaction == 1485) {
        // ESADimensionDo's end (sub_5B9BF0): the target here -> its layer back (vtbl+72 -> the sectors' sub_7EF120:
        // its appear, S2C 8, to the players around — a corpse stays where it is), the condition 15 off
        if (m.banished && m.alive) {
            m.banished = false;
            broadcast_entity_spawn(m.object_id, 0x0008);
        }
        if (m.cond_test(15)) m.cond_remove(15);
    }
    if (monster_es_needs_caster(e.esaction)) {
        if (e.result == 1) monster_recompute(m);
        // the caster's vtbl+132(target, 5000): it marks the monster when it has no owner yet
        if (m.alive && !m.owner_oid && e.caster && player_session(e.caster)) {
            m.owner_oid = e.caster;
            m.owner_hp  = m.hp;
            PacketBuffer p(static_cast<Opcode>(0x016D), janus::PaulaCrypt::Instance().IsLoaded());
            p.write<uint16_t>(static_cast<uint16_t>(m.object_id));
            p.write<uint16_t>(static_cast<uint16_t>(e.caster));
            broadcast_zone(m.zone, p, 0);
        }
    } else if (e.esaction == 1324 && e.result == 1) {
        m.resist_buff[8] = static_cast<int16_t>(m.resist_buff[8] + e.value);   // sub_580490(0): PhysicalR
        m.resist_buff[9] = static_cast<int16_t>(m.resist_buff[9] + e.value);   // GunR
    } else if (e.esaction == 1119 && e.result == 1) {
        if (m.cond_test(15)) m.cond_remove(15);              // sub_555650 (a player caster: nothing more)
    } else if (e.esaction == 1141 && e.result == 1) {
        if (m.cond_test(16)) m.cond_remove(16);              // sub_5599C0
    } else if (e.esaction == 1144 && e.result == 1 && m.alive) {
        if (m.cond_test(15)) m.cond_remove(15);              // sub_559450 (not on a dead one)
    } else if (e.esaction == 1471) {
        if (m.cond_test(15)) m.cond_remove(15);              // sub_5B72B0 (whatever the roll)
    } else if (e.esaction == 1183 && e.result == 1 && m.alive) {
        if (m.cond_test(1)) m.cond_remove(1);                // sub_564880 (not on a dead one)
    } else if (e.esaction == 1315 && e.result == 1) {
        // ESAIceCrystal's release (sub_57EC00): the ElecR back — even when a DebuffSet resisted its cut (retail) —,
        // the condition 1 off, the caster's mark with 5000
        m.resist_buff[4] = static_cast<int16_t>(m.resist_buff[4] + e.value);
        if (m.cond_test(1)) m.cond_remove(1);
        if (m.alive && !m.owner_oid && e.caster && player_session(e.caster)) mark_owner_of(m, e.caster);
    } else if (e.esaction == 1355 && e.result == 1) {
        // ESAChillingPrison's end (sub_585220): the condition 15 and the +0x148 flag 32 off, the caster's mark
        if (m.cond_test(15)) m.cond_remove(15);
        m.skill_flags = static_cast<uint16_t>(m.skill_flags & ~32u);
        if (m.alive && !m.owner_oid && e.caster && player_session(e.caster)) mark_owner_of(m, e.caster);
    } else if (e.esaction == 1359 && e.result == 1) {      // sub_585700(1): the speeds back
        const double p = static_cast<double>(e.ratio);
        m.walk_speed  = static_cast<float>(m.walk_speed  + p * 100.0);
        m.run_speed   = static_cast<float>(m.run_speed   + p * 160.0);
        m.chase_speed = static_cast<float>(m.chase_speed + p * 130.0);
    } else if (e.esaction == 1288) {
        if (m.cond_test(0x20000)) m.cond_remove(0x20000);    // sub_5715E0
    } else if (e.esaction == 1221 || e.esaction == 1272) {
        // ESAChargingHit / ESAPowerChargingDo's end (sub_569940 / sub_5729D0): applied, on a live one (+92 bit 4 clear):
        // the condition 1 off (a monster caster's vtbl+136: none here)
        if (e.result == 1 && m.alive && m.cond_test(1)) m.cond_remove(1);
    } else if (e.esaction == 1281) {
        // ESAFullHouseHit's end (sub_5702D0): its hit (+224) not missed / blocked / 6 / 8, applied: the condition 15 off
        const int r = e.value;
        if (r != 2 && r != 1 && r != 6 && r != 8 && e.result == 1 && m.cond_test(15)) m.cond_remove(15);
    } else if (e.esaction == 1453 || e.esaction == 1456) {
        monster_recompute(m);                                // ESAGMCheer*Do's end (sub_596EF0): the adder out
    } else if (e.esaction == 1526 && e.result == 1 && m.alive) {
        if (m.cond_test(15)) m.cond_remove(15);              // ESAMentalBreak (skill_bear.cpp): the stun off
    } else if (e.esaction == 1531 && m.alive) {
        if (m.cond_test(1)) m.cond_remove(1);                // ESAQuakeHammerDo: the paralysis off
    } else if (e.esaction == 1537 && e.result == 1) {
        monster_recompute(m);                                // ESABbongHammer: its DP cut out
    }
    if (e.esaction == 1543 && e.result == 1) {               // ESABearAttack: the speeds back (its DP: above)
        const double p = static_cast<double>(e.ratio2);
        m.walk_speed  = static_cast<float>(m.walk_speed  + p * 100.0);
        m.run_speed   = static_cast<float>(m.run_speed   + p * 160.0);
        m.chase_speed = static_cast<float>(m.chase_speed + p * 130.0);
    }
    LOG_DEBUG("cGameServer: ES %u ESAction %d on mob %u ended", e.id, e.esaction, static_cast<unsigned>(m.object_id));
}

// The zone tick of the monsters' ESs (each class's vtbl+56 and the base tick sub_50FF90; the 10 s
// warning S2C 850 goes to the target — a monster has no client). A dead monster's ESs end.
// Hardening (not retail): retail's loop ticks on regardless; a throw here is logged so item_tick still re-arms.
void cGameServer::monster_es_tick(int32_t elapsed_ms)
try {
    std::lock_guard<std::recursive_mutex> lk(mob_es_mutex_);
    for (uint32_t oid : entity_mgr_.monsters_with_es()) {
        MonsterEntity* m = entity_mgr_.get_monster(oid);
        if (!m) continue;
        for (size_t i = 0; i < m->es.size();) {
            MonsterEntity::Es& e = m->es[i];
            if (e.esaction == 1119) {                        // ESASkunkBag's tick (sub_5554B0)
                if (!m->alive) { monster_es_end(*m, i); continue; }
                e.elapsed_ms += elapsed_ms;
                if (e.phase == 0) {
                    if (e.elapsed_ms >= e.phase_ms) { e.phase = 1; e.phase_ms = 100; e.elapsed_ms = 0; }
                } else if (e.phase == 1) {
                    if (e.elapsed_ms >= e.phase_ms) {
                        e.phase = 2; e.phase_ms = 100; e.elapsed_ms = 0;
                        if (e.result == 1) {                 // sub_5555E0: the target stops, condition 15
                            const auto now = std::chrono::steady_clock::now();
                            int32_t x, y;
                            m->motion.pos(now, x, y);
                            m->motion.rest(x, y);
                            m->pos_x = x; m->pos_y = y;
                            m->cond_add(15);
                        }
                    }
                } else {
                    if (e.result != 1) { monster_es_end(*m, i); continue; }
                    if (e.elapsed_ms >= e.end_ms || !player_session(e.caster)) { monster_es_end(*m, i); continue; }
                }
                if (e.result != 1 && !e.stay) { monster_es_end(*m, i); continue; }
                ++i;
                continue;
            }
            if (e.esaction == 1354 || e.esaction == 1359) {  // sub_584700 / sub_585AF0
                e.elapsed_ms += elapsed_ms;
                GameClientSession* c = player_session(e.caster);
                if (!c) { monster_es_end(*m, i); continue; }
                if (e.esaction == 1359 && e.elapsed_ms >= e.end_ms) { monster_es_end(*m, i); continue; }
                if (e.esaction == 1354 && e.elapsed_ms >= 2000) {
                    e.elapsed_ms -= 2000;
                    c->field_burn_pulse(static_cast<uint16_t>(m->object_id), e.level);
                }
                if (m->es[i].result != 1) { monster_es_end(*m, i); continue; }   // the base tick
                ++i;
                continue;
            }
            if (e.esaction == 1319) {                        // ESAFireBurnDo's tick (sub_5B6910)
                e.elapsed_ms += elapsed_ms;
                GameClientSession* c = player_session(e.caster);
                if (!c || !m->alive || e.elapsed_ms >= e.end_ms || e.phase == 1) { monster_es_end(*m, i); continue; }
                if (e.elapsed_ms >= 2000) {
                    e.elapsed_ms -= 2000;
                    e.end_ms     -= 2000;
                    c->burn_pulse(static_cast<uint16_t>(m->object_id), e.value, e.skill);
                    if (const MobDef* md = mob_db_.get(static_cast<int>(m->mob_id)))
                        if (md->type == 13) m->es[i].phase = 1;
                }
                if (m->es[i].result != 1) { monster_es_end(*m, i); continue; }   // the base tick
                ++i;
                continue;
            }
            if (e.esaction == 1316) {                        // ESAElectricField's tick (sub_5B6360)
                e.elapsed_ms += elapsed_ms;
                e.pulse_ms   += elapsed_ms;
                GameClientSession* c = player_session(e.caster);
                if (!c || e.elapsed_ms >= e.end_ms) { monster_es_end(*m, i); continue; }
                if (e.pulse_ms >= 2000) {
                    e.pulse_ms = 0;
                    c->field_pulse(m->es[i]);
                }
                if (m->es[i].result != 1) { monster_es_end(*m, i); continue; }   // the base tick
                ++i;
                continue;
            }
            if (e.esaction == 1471) {                        // ESAElectricSwordDo's tick (sub_5B7240): the time only
                e.elapsed_ms += elapsed_ms;
                if (e.elapsed_ms >= e.end_ms) { monster_es_end(*m, i); continue; }
                ++i;
                continue;
            }
            if (e.esaction >= 1370 && e.esaction <= 1378 && e.esaction % 2 == 0) {   // ESA*AreaDo (sub_58A460)
                e.elapsed_ms += elapsed_ms;
                GameClientSession* c = player_session(e.caster);
                if (!c || e.elapsed_ms >= e.end_ms) { monster_es_end(*m, i); continue; }
                if (e.elapsed_ms >= 2000) {
                    e.elapsed_ms -= 2000;
                    const int k = e.phase - 11;
                    if (k >= 0 && k < 8 && e.phase_ms != m->resist[k]) {
                        // the target's resist changed: the cut back (sub_58A650(1)), made anew (sub_58A560), taken
                        // again (sub_58A650(0))
                        m->resist_buff[k] = static_cast<int16_t>(m->resist_buff[k] + e.value);
                        float v = static_cast<float>((static_cast<double>(c->stat_value(4)) / 400.0 +
                                                      0.300000011920929) * static_cast<double>(e.ratio));
                        if (v > 0.5f) v = 0.5f;
                        e.phase_ms = m->resist[k];
                        e.value    = static_cast<int32_t>(static_cast<double>(e.phase_ms) * static_cast<double>(v));
                        if (e.value <= 0) e.value = 1;
                        m->resist_buff[k] = static_cast<int16_t>(m->resist_buff[k] - e.value);
                    }
                }
                if (e.result != 1) { monster_es_end(*m, i); continue; }   // the base tick
                ++i;
                continue;
            }
            if (e.esaction == 1155) {                        // ESABindDo's tick (sub_55B0A0): its time; kept unapplied
                e.elapsed_ms += elapsed_ms;
                if (e.elapsed_ms >= e.end_ms) { monster_es_end(*m, i); continue; }
                ++i;
                continue;
            }
            if (e.esaction == 1178) {                        // ESACruelFlameDo's tick (sub_561360)
                e.elapsed_ms += elapsed_ms;
                e.pulse_ms   += elapsed_ms;
                GameClientSession* c = player_session(e.caster);
                if (e.elapsed_ms > e.end_ms || !c || e.phase == 1) { monster_es_end(*m, i); continue; }
                if (e.pulse_ms > 2000) {
                    c->cruel_burn_pulse(static_cast<uint16_t>(m->object_id), e.id);
                    e.pulse_ms = 0;
                    if (const MobDef* md = mob_db_.get(static_cast<int>(m->mob_id)))
                        if (md->type == 13) e.phase = 1;
                }
                if (e.result != 1) { monster_es_end(*m, i); continue; }   // the base tick
                ++i;
                continue;
            }
            if (e.esaction == 1322) {                        // ESADarkSlow's tick (sub_57FB10)
                e.elapsed_ms += elapsed_ms;
                if (e.elapsed_ms >= e.end_ms || !player_session(e.caster)) { monster_es_end(*m, i); continue; }
                if (e.result != 1) { monster_es_end(*m, i); continue; }   // the base tick
                ++i;
                continue;
            }
            if (e.esaction == 1279) {                        // ESAFearDo's tick (sub_56F2F0)
                e.elapsed_ms += elapsed_ms;
                if (e.elapsed_ms >= e.end_ms) { monster_es_end(*m, i); continue; }
                if (e.result != 1) { monster_es_end(*m, i); continue; }   // the base tick
                ++i;
                continue;
            }
            if (e.esaction == 1332) {                        // ESAUnableRecoveryDo's tick (sub_526DB0)
                e.elapsed_ms += elapsed_ms;
                if (e.elapsed_ms >= e.end_ms) { monster_es_end(*m, i); continue; }
                if (e.result != 1) { monster_es_end(*m, i); continue; }   // the base tick
                ++i;
                continue;
            }
            if (e.esaction == 1485) {                        // ESADimensionDo's tick (sub_5B9B80): its time (the
                e.elapsed_ms += elapsed_ms;                  //   target gone takes the ES with it); applied always
                if (e.elapsed_ms >= e.end_ms) { monster_es_end(*m, i); continue; }
                ++i;
                continue;
            }
            if (e.esaction == 1273) {                        // ESAWinkOfCharm's tick (sub_56EEE0): +200 past +196 -> the
                e.elapsed_ms += elapsed_ms;                  //   end; the base tick (not applied: the end)
                if (e.elapsed_ms >= e.end_ms || e.result != 1) { monster_es_end(*m, i); continue; }
                ++i;
                continue;
            }
            if (e.esaction == 1478) {
                // ESABuffCanclerDo's tick (sub_5B81D0): the target or the caster gone, or no Count left, the end; every
                // Delay (+196) the pulse (buff_cancler_hit); the base tick (kept whatever the roll).
                GameClientSession* c = player_session(e.caster);
                if (!c || e.phase <= 0) { monster_es_end(*m, i); continue; }
                e.elapsed_ms += elapsed_ms;
                if (e.elapsed_ms >= e.phase_ms) {
                    e.elapsed_ms = 0;
                    c->buff_cancler_pulse(static_cast<uint16_t>(m->object_id), e.id);
                }
                ++i;
                continue;
            }
            if (e.esaction == 1350 || e.esaction == 1384) {
                // ESAPoisonBaggageDo (sub_583FC0): the target or the caster gone, or past +260, the end; every 1000 ms
                // (+264) a poison hit. ESAPoisonAssaultDo (sub_58D030): the target gone or dead, no caster, past +224,
                // or its stop flag (+240), the end; every DamagePerSec s (+232 <= +228) a hit — a Type-13 target then
                // takes the stop flag. Then the base tick (not applied: the end).
                e.elapsed_ms += elapsed_ms;
                e.pulse_ms   += elapsed_ms;
                GameClientSession* c = player_session(e.caster);
                const bool gone = !c || (e.esaction == 1384 && (!m->alive || e.phase == 1));
                if (gone || e.elapsed_ms >= e.end_ms) { monster_es_end(*m, i); continue; }
                const int32_t gap = (e.esaction == 1350) ? 1000 : e.phase_ms;
                if (e.pulse_ms >= gap) {
                    e.pulse_ms = 0;
                    c->poison_pulse(static_cast<uint16_t>(m->object_id), e.id);
                    if (e.esaction == 1384)
                        if (const MobDef* md = mob_db_.get(static_cast<int>(m->mob_id)))
                            if (md->type == 13) m->es[i].phase = 1;
                }
                if (m->es[i].result != 1) { monster_es_end(*m, i); continue; }   // the base tick
                ++i;
                continue;
            }
            if (e.esaction == 1537) {
                // ESABbongHammer (Mole Hammer, skill_bear.cpp — modelled on the poison pulse above): the target dead, the
                // caster gone or past its time, the end; every TicTime ms (+phase_ms) a hit on the caster's strand.
                e.elapsed_ms += elapsed_ms;
                e.pulse_ms   += elapsed_ms;
                GameClientSession* c = player_session(e.caster);
                if (!c || !m->alive || e.elapsed_ms >= e.end_ms) { monster_es_end(*m, i); continue; }
                if (e.phase_ms > 0 && e.pulse_ms >= e.phase_ms) {
                    e.pulse_ms = 0;
                    c->mole_pulse(static_cast<uint16_t>(m->object_id), e.id);
                }
                ++i;
                continue;
            }
            if (e.esaction == 1221 || e.esaction == 1272) {
                // ESAChargingHit (sub_569740) / ESAPowerChargingDo (sub_5727A0): 1221 not applied with its caster gone,
                // 1272 not applied or its caster gone -> the end; a dead target -> the end; before 1000 / 3000 ms the base
                // tick (not applied: the end); then it lands at +220 (sub_7149C0) and ends.
                e.elapsed_ms += elapsed_ms;
                const bool caster = player_session(e.caster) != nullptr;
                if (e.esaction == 1221 ? (e.result != 1 && !caster) : (e.result != 1 || !caster)) {
                    monster_es_end(*m, i);
                    continue;
                }
                if (!m->alive) { monster_es_end(*m, i); continue; }
                if (e.elapsed_ms < (e.esaction == 1221 ? 1000 : 3000)) {
                    if (e.result != 1) { monster_es_end(*m, i); continue; }
                    ++i;
                    continue;
                }
                knock_land(*m, e);
                monster_es_end(*m, i);
                continue;
            }
            if (e.esaction == 1348) {                        // ESAMassiveAttack (sub_583040): not applied or its caster
                e.elapsed_ms += elapsed_ms;                  //   gone -> the end; past 1000 ms it lands at +220
                if (e.result != 1 || !player_session(e.caster)) { monster_es_end(*m, i); continue; }
                if (e.elapsed_ms < 1000) { ++i; continue; }
                knock_land(*m, e);
                monster_es_end(*m, i);
                continue;
            }
            if (e.esaction == 1281) {                        // ESAFullHouseHit (sub_5701F0): its caster gone or past
                e.elapsed_ms += elapsed_ms;                  //   +220 -> the end; the base tick (not applied: the end)
                if (!player_session(e.caster) || e.elapsed_ms >= e.end_ms || e.result != 1) {
                    monster_es_end(*m, i);
                    continue;
                }
                ++i;
                continue;
            }
            if (e.esaction == 1382) {
                // ESASummonSoldier (sub_58C9E0): +196 -= the time; its caster gone -> the end; above 0: at <= 1000 once
                // the hit (soldier_hit on the caster's strand), the base tick; else the end.
                e.phase_ms = static_cast<int16_t>(e.phase_ms - elapsed_ms);
                GameClientSession* c = player_session(e.caster);
                if (!c) { monster_es_end(*m, i); continue; }
                if (e.phase_ms > 0) {
                    if (e.phase_ms <= 1000 && !e.phase) {
                        e.phase = 1;
                        c->soldier_pulse(static_cast<uint16_t>(m->object_id), e.id);
                    }
                    if (e.result != 1) { monster_es_end(*m, i); continue; }
                    ++i;
                    continue;
                }
                e.phase_ms = 0;
                monster_es_end(*m, i);
                continue;
            }
            e.elapsed_ms += elapsed_ms;
            if (!m->alive || e.elapsed_ms >= e.end_ms) { monster_es_end(*m, i); continue; }
            // (ESASleepVoice's tick sub_54D9E0 only counts its time: without its caster it stays, adding nothing;
            // ESADefenseParalysis / ESAPowerStun end without theirs, sub_559900 / sub_559370)
            if (((monster_es_needs_caster(e.esaction) && e.esaction != 1096) || e.esaction == 1141 ||
                 e.esaction == 1144 || e.esaction == 1183 || e.esaction == 1315 || e.esaction == 1355 ||
                 e.esaction == 1526) &&
                !player_session(e.caster)) {
                monster_es_end(*m, i);
                continue;
            }
            if (e.result != 1) { monster_es_end(*m, i); continue; }
            if (!e.warned) {
                e.life_ms -= elapsed_ms;
                if (e.life_ms <= kEsWarnMs) { e.life_ms = 0; e.warned = true; }
            }
            ++i;
        }
    }
} catch (const std::exception& ex) {
    LOG_ERROR("cGameServer: monster_es_tick: %s", ex.what());
} catch (...) {
    LOG_ERROR("cGameServer: monster_es_tick: unknown exception");
}

// sub_524F60 -> sub_524690 on the selected monster.
void GameClientSession::es_apply_to_monster(const SkillDef& d, uint8_t level, const EsaRow* act)
{
    MonsterEntity* m = su_.target ? server_.entity_mgr().get_monster(su_.target) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    es_apply_on(*m, d, level, act);
}

// Player vtbl+164 (sub_76CF50): may we attack this monster? Without an owner (+1212) yes; an owned one of
// MonsterParamEx2 Type 3 / 4 / 6 / 7 / 9 / 10 / 11 (sub_716E30) too; else its owner — or, in a party, a
// member of our share group (the ResChain 5 PartyEnemyJudge, sub_86DB90). (The no-penalty flag +4684 is
// never set here.)
bool GameClientSession::may_attack_owned(const MonsterEntity& m) const
{
    if (!m.owner_oid || m.owner_oid == object_id_) return true;
    if (const MobDef* md = server_.mob_db().get(static_cast<int>(m.mob_id))) {
        const int t = md->type;
        if (t == 3 || t == 4 || t == 6 || t == 7 || t == 9 || t == 10 || t == 11) return true;
    }
    if (const PlayerEntity* op = server_.entity_mgr().get_player(m.owner_oid))
        if (server_.party().same_group(char_uid_, op->char_uid)) return true;
    return false;
}

// The area skills' target test (e.g. sub_5C9A10): a monster with no owner (sub_4269D0), ours, or one whose
// owner's share group holds us (the owner's PartyMemberJudge, sub_716EF0) may be hit — else it is skipped.
bool GameClientSession::owned_by_other(const MonsterEntity& m) const
{
    if (!m.owner_oid || m.owner_oid == object_id_) return false;
    const PlayerEntity* op = server_.entity_mgr().get_player(m.owner_oid);
    return op && !server_.party().same_group(op->char_uid, char_uid_);
}

// sub_524690(zone, the monster, the action, 1) for an ES of ours on a monster.
void GameClientSession::es_apply_on(MonsterEntity& mref, const SkillDef& d, uint8_t level, const EsaRow* act)
{
    MonsterEntity* m = &mref;
    if (m->zone != zone_id_) return;
    const bool area_do = (d.esaction >= 1370 && d.esaction <= 1378 && d.esaction % 2 == 0);
    if (!monster_es_needs_caster(d.esaction) && d.esaction != 1324 && d.esaction != 1119 && d.esaction != 1141 &&
        d.esaction != 1144 && d.esaction != 1183 && d.esaction != 1315 && d.esaction != 1316 && d.esaction != 1355 &&
        d.esaction != 1319 && d.esaction != 1354 && d.esaction != 1359 && d.esaction != 1485 && !area_do &&
        d.esaction != 1155 && d.esaction != 1178 && d.esaction != 1322 && d.esaction != 1332 && d.esaction != 1279 &&
        d.esaction != 1350 && d.esaction != 1384 && d.esaction != 1273 && d.esaction != 1478 && d.esaction != 1221 &&
        d.esaction != 1272 && d.esaction != 1348 && d.esaction != 1281 && d.esaction != 1382 && d.esaction != 1453 &&
        d.esaction != 1456 && d.esaction != 1526 && d.esaction != 1537) {
        LOG_DEBUG("GameClientSession %u: skill %u ESAction %d (%s) on a monster — not implemented yet",
                  session_id(), d.id, d.esaction, d.esa_table.c_str());
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
    if (d.esclass != -1 && d.type != 2 && d.id != 7046) {
        if (!server_.monster_es_admit(*m, d)) return;
    }
    MonsterEntity::Es e;
    e.id       = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
    e.esaction = d.esaction;
    e.estate   = d.estate;
    e.esclass  = d.esclass;
    e.skill    = static_cast<uint16_t>(d.id);
    e.flags    = es_flags(d);
    e.caster   = object_id_;
    e.added    = tick_ms();
    float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    if (!esa_row_has_apply_ratio(d.esaction)) apply_ratio = 0.0f;
    e.result = apply_roll(apply_ratio, 0);                  // (a monster's +1256 resist map is empty)
    const int32_t seconds = static_cast<int16_t>(act ? static_cast<int>(act->get("Seconds", 0.0)) : 0);
    e.ratio   = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    e.end_ms  = 1000 * seconds;
    e.life_ms = e.end_ms;
    if (d.esaction == 1119) {
        // ESASkunkBag (sub_555200; ESAction_SkunkBag: Seconds +4): +220 = 1000 (the throw), +224 = 1000 *
        // Seconds (the stun), the lifetime 1000 * +224 (retail: 1000 times again); the target's Prevent Stun
        // marker (1398) or protection (+92 flag 1) turn an applied roll into 2 — monsters have neither; not
        // applied -> kept anyway (+184 = 1) until its stun phase.
        e.phase    = 0;
        e.phase_ms = 1000;
        e.life_ms  = 1000 * e.end_ms;
        e.stay     = (e.result != 1);
    }
    if (d.esaction == 1324 && (!gun_in_any_slot() || !mag_.item)) {
        skill_cancel_reason(18);                             // sub_5800F0: S2C 239 [18], no object
        server_.entity_mgr().free_oid(e.id);
        return;
    }
    e.resisted = es_resisted_;                               // +192: the parts a DebuffSet resisted
    e.level    = level;
    if (d.esaction == 1354 || d.esaction == 1359) {
        // ESAFireFieldDo / ESABreakdownGroundDo (sub_5845B0 / sub_585580; the action carries the field's row): +264 =
        // 1000 * Seconds, the lifetime; the block {S2C 224 sub_5EDE00 [us][target][ES id]..., no tail — VERIFIED: the
        // clients' ESAFireFieldDo 0x509FC0 / ESABreakdownGroundDo 0x50B5E0 read none — and FireFieldDo's first burn};
        // BreakdownGroundDo applied: the target's walk / run / chase speeds (+156 / +160 / +164) -= SpeedDown / 100 *
        // 100 / 160 / 130 (sub_585700(0)), back at its end. Their ticks (sub_584700 / sub_585AF0): us or the target
        // gone -> the end; FireFieldDo every 2000 ms a burn (its +8 counted back, so never its time: the field ends
        // it); BreakdownGroundDo past +264 the end; the base tick (not applied: the end).
        e.end_ms  = 1000 * static_cast<int16_t>(act ? static_cast<int>(act->get("Seconds", 0.0)) : 0);
        e.life_ms = e.end_ms;
        {
            std::vector<PacketBuffer> group;
            PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
            o.write<uint16_t>(static_cast<uint16_t>(object_id_));
            o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
            o.write<uint16_t>(e.id);
            o.write<uint16_t>(static_cast<uint16_t>(e.esaction));
            o.write<uint8_t>(kEsKind);
            o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
            o.write<uint16_t>(e.skill);
            o.write<uint16_t>(0);
            group.push_back(std::move(o));
            server_.broadcast_block(zone_id_, group, 0);
        }
        if (d.esaction == 1359 && e.result == 1) {
            const double p = static_cast<double>(static_cast<int16_t>(act ? static_cast<int>(act->get("SpeedDown", 0.0)) : 0)) / 100.0;
            e.ratio = static_cast<float>(p);
            m->walk_speed  = static_cast<float>(m->walk_speed  - p * 100.0);
            m->run_speed   = static_cast<float>(m->run_speed   - p * 160.0);
            m->chase_speed = static_cast<float>(m->chase_speed - p * 130.0);
        }
        m->es.push_back(e);
        if (d.esaction == 1354) field_burn(static_cast<uint16_t>(m->object_id), level);
        return;
    }
    if (d.esaction == 1319) {
        // ESAFireBurnDo (sub_5B67A0; the action carries Phoenix Rising's row: ApplyRatio +0, AP +4, Radius +8, Seconds
        // +10): +264 = 1000 * Seconds; the damage made once (sub_5B6710: the magic damage sub_5E6270(AP + 3 * our Fire,
        // 1.0, MA offset 49, element 1 = Fire), +260 = its value); the lifetime +264; the block {S2C 224 sub_5EDE00
        // [us][target][ES id][1319][2][ESClass 1][2124][0], no tail — VERIFIED: the client's ESAFireBurnDo 0x4FCA70
        // reads none —, the first burn (burn_hit)}. Its tick (sub_5B6910): us or the target gone, past +264, or its
        // stop flag -> the end; every 2000 ms (+264 counted down with it) a burn — a monster of Type 13 stops after
        // it —; the base tick (not applied: the end).
        e.end_ms   = 1000 * static_cast<int16_t>(act ? static_cast<int>(act->get("Seconds", 0.0)) : 0);
        e.life_ms  = e.end_ms;
        const int32_t base = (act ? static_cast<int32_t>(act->get("AP", 0.0)) : 0) + 3 * elem_slot(0);
        e.value    = skill_magic_damage(*m, base, 1.0f, 49, 1).dmg;
        e.phase    = 0;                                      // +268: the stop flag
        {
            std::vector<PacketBuffer> group;
            PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
            o.write<uint16_t>(static_cast<uint16_t>(object_id_));
            o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
            o.write<uint16_t>(e.id);
            o.write<uint16_t>(1319);
            o.write<uint8_t>(kEsKind);
            o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
            o.write<uint16_t>(e.skill);
            o.write<uint16_t>(0);
            group.push_back(std::move(o));
            server_.broadcast_block(zone_id_, group, 0);
        }
        m->es.push_back(e);
        burn_hit(static_cast<uint16_t>(m->object_id), e.value, e.skill);
        return;
    }
    if (d.esaction == 1315 || d.esaction == 1316 || d.esaction == 1355) {   // inits of their own (skill_bard.cpp)
        if (!bard_debuff_on(*m, e, d, act)) server_.entity_mgr().free_oid(e.id);
        return;
    }
    if (area_do) {                                           // ESA*AreaDo (sub_58A890, skill_bard.cpp)
        if (!area_do_on(*m, e, act)) server_.entity_mgr().free_oid(e.id);
        return;
    }
    if (d.esaction == 1485) {                                // ESADimensionDo (sub_5B99A0, skill_bard.cpp)
        if (!dimension_do_on(*m, e)) server_.entity_mgr().free_oid(e.id);
        return;
    }
    if (d.esaction == 1155 || d.esaction == 1178 || d.esaction == 1322 || d.esaction == 1332 || d.esaction == 1279) {
        bool made = false;                                   // (skill_dragon.cpp)
        if (d.esaction == 1155)      made = bind_do_on(*m, e);
        else if (d.esaction == 1279) made = fear_on(*m, e, act);
        else if (d.esaction == 1178) made = cruel_flame_on(*m, e, act);
        else if (d.esaction == 1322) made = dark_slow_on(*m, e, act);
        else                         made = unable_recovery_on(*m, e, act);
        if (!made) server_.entity_mgr().free_oid(e.id);
        return;
    }
    if (d.esaction == 1350 || d.esaction == 1384) {          // ESAPoisonBaggageDo / ESAPoisonAssaultDo (skill_thief.cpp)
        if (!poison_do_on(*m, e, act)) server_.entity_mgr().free_oid(e.id);
        return;
    }
    if (d.esaction == 1273 || d.esaction == 1478) {          // ESAWinkOfCharm / ESABuffCanclerDo (skill_cat.cpp)
        const bool made = (d.esaction == 1273) ? wink_on(*m, e, act) : buff_cancler_on(*m, e, act);
        if (!made) server_.entity_mgr().free_oid(e.id);
        return;
    }
    if (d.esaction == 1221 || d.esaction == 1272 || d.esaction == 1348 || d.esaction == 1281 || d.esaction == 1382) {
        if (!card_es_on(*m, e, d, act)) server_.entity_mgr().free_oid(e.id);   // (skill_card.cpp)
        return;
    }
    if (d.esaction == 1453 || d.esaction == 1456) {          // a GM's cheer on a monster (skill_gm.cpp)
        if (!gm_cheer_on(*m, e, act)) server_.entity_mgr().free_oid(e.id);
        return;
    }
    if (d.esaction == 1526 || d.esaction == 1537 || d.esaction == 1543) {   // the Bear's debuffs (skill_bear.cpp)
        if (!bear_debuff_on(*m, e, d, act)) server_.entity_mgr().free_oid(e.id);
        return;
    }
    if (d.esaction == 1243) {
        // ESAAdhesiveDo (sub_56A680; the action carries Sticky Icky's row: HvDRatio +4, LkDRatio +8, DpDRatio +12,
        // Seconds +16): the three ratios, +224 = 1000 * Seconds, +8 = 0, its caster (+228); the caster must be
        // able to attack the target (sub_5E6D70) — else no object; the block {S2C 224 sub_5EDE00 [us][target][ES
        // id]... + [u8 +180]} (VERIFIED: the client's ESAAdhesiveDo 0x4E65C0 reads a u8); we mark the target
        // (vtbl+132); applied -> the adder (monster_recompute), the update, the skill's TM per hit. Its tick
        // (sub_56ABA0): past +224, or the owner / caster gone, the end; its end (sub_56A9A0): the adder off and
        // the caster's mark with 5000 (monster_es_needs_caster).
        e.ratio  = act ? static_cast<float>(act->get("HvDRatio", 0.0)) : 0.0f;
        e.ratio2 = act ? static_cast<float>(act->get("LkDRatio", 0.0)) : 0.0f;
        e.ratio3 = act ? static_cast<float>(act->get("DpDRatio", 0.0)) : 0.0f;
    }
    if (d.esaction == 1189 && e.result != 1) {               // ESAWaterNetDo sub_564B80: not applied -> no object
        server_.entity_mgr().free_oid(e.id);
        return;
    }
    if (d.esaction == 1183) {
        // ESAElectricShockDo (sub_5644F0; ESAction_ElectricShock: Seconds +4): +220 = 1000 * Seconds; the target's
        // Prevent Stun (1398) -> no object; applied -> the lifetime 1000 * +220 (retail: 1000 times again).
        for (const MonsterEntity::Es& o : m->es)
            if (o.esaction == 1398) { server_.entity_mgr().free_oid(e.id); return; }
        if (e.result == 1) e.life_ms = 1000 * e.end_ms;
    }
    if (!m->base_set) {                                      // the base stats (+704)
        m->base_set = true;
        m->base_ap = m->ap; m->base_ac = m->ac; m->base_dx = m->dx; m->base_da = m->da;
        m->base_lk = m->lk; m->base_dp = m->dp; m->base_hv = m->hv;
        m->base_ma = m->ma; m->base_md = m->md;
    }
    // S2C 224 (sub_5EDE00) [caster][target][ES id]... + [u8 result], to the area of both.
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(static_cast<uint16_t>(e.esaction));
        o.write<uint8_t>(kEsKind);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        if (e.esaction == 1081 || e.esaction == 1096)            // ESADpDown / ESASleepVoice: [u16 the payload's
            o.write<uint16_t>(su_.target);                       //   target][u8] (sub_54A550 / sub_54D570)
        o.write<uint8_t>(static_cast<uint8_t>(e.result));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    m->es.push_back(e);
    scroll_use_up(d.esclass, d.esaction);                   // sub_524690 (a4 = 1): a scroll's class only
    if (d.esaction != 1324) mark_monster_owner(*m);         // the caster is not the target: vtbl+132
    LOG_DEBUG("GameClientSession %u: ES %u ESAction %d (skill %u lv %u) on mob %u result %d ratio %.2f %d ms",
              session_id(), e.id, e.esaction, e.skill, level, static_cast<unsigned>(m->object_id), e.result, e.ratio,
              e.end_ms);
    if (e.result != 1) return;
    if (d.esaction == 1324) {
        MonsterEntity::Es& me = m->es.back();
        me.value = static_cast<int32_t>((static_cast<double>(m->da) * me.ratio + 20.0) / 3.0);
        if (me.value > 80) me.value = 80;
        const int32_t cut = me.value;
        m->resist_buff[8] = static_cast<int16_t>(m->resist_buff[8] - cut);   // PhysicalR (+964)
        m->resist_buff[9] = static_cast<int16_t>(m->resist_buff[9] - cut);   // GunR (+962)
        for (size_t i = 0; i < m->es.size();) {                              // sub_5803C0
            const int a = m->es[i].esaction;
            if (a == 1118 || a == 1130 || a == 1129 || a == 1311) server_.monster_es_end(*m, i);
            else ++i;
        }
        consume_bullets(1);
        LOG_DEBUG("GameClientSession %u: Shield Breaker cuts mob %u Physical/Gun resist by %d", session_id(),
                  static_cast<unsigned>(m->object_id), cut);
        return;
    }
    if (d.esaction == 1119) {                                // the stun comes from its tick (monster_es_tick)
        skill_hit_tm(*m, e.skill);                           // sub_891C80 (applied, a player caster)
        return;
    }
    if (d.esaction == 1141 || d.esaction == 1144) {
        // ESADefenseParalysis (Guard Break, sub_559710) / ESAPowerStun (Faint, sub_559150): applied -> the skill's
        // TM per hit, the target stops (sub_41C5D0) and takes the condition 16 (it neither dodges nor blocks a
        // melee blow, sub_5E5700) / 15 (the stun: no move, no attack) — sub_559980 / sub_5593F0.
        skill_hit_tm(*m, e.skill);
        const auto now = std::chrono::steady_clock::now();
        int32_t x, y;
        m->motion.pos(now, x, y);
        m->motion.rest(x, y);
        m->pos_x = x; m->pos_y = y;
        m->cond_add(d.esaction == 1141 ? 16 : 15);
        return;
    }
    if (d.esaction == 1183) {
        // ESAElectricShockDo applied: the skill's TM per hit, then (sub_564820) the target stops and takes the
        // condition 1 (no MoveTo) — unless a Prevent Stun (1398) is on it.
        skill_hit_tm(*m, e.skill);
        const auto now = std::chrono::steady_clock::now();
        int32_t x, y;
        m->motion.pos(now, x, y);
        m->motion.rest(x, y);
        m->pos_x = x; m->pos_y = y;
        m->cond_add(1);
        return;
    }
    server_.monster_recompute(*m);
    LOG_DEBUG("GameClientSession %u: mob %u now DP=%d HV=%d (base %d/%d)", session_id(),
              static_cast<unsigned>(m->object_id), m->dp, m->hv, m->base_dp, m->base_hv);
    skill_hit_tm(*m, e.skill);                               // sub_891C80
}

// sub_5DE250: Lightning Strike's stun on one of its targets (dead or not — sub_5DE1A0 does not look). A monster
// with a DebuffSet first rolls against the strike's DebuffBitCode (sub_891F80; all resisted -> S2C 1247, nothing
// more); else the action {ESAction 1471, EState 0, ESClass 1, flags 2, the strike's skill and row} through
// sub_524690(.., 1) — the skill is an active one (Type 2): no limiter. ESAElectricSwordDo (InitESAction's apply
// roll, then the init sub_5B70F0): its time (+196) = 1000 * StunTime (the row's +12), kept whatever the roll
// (+184 = 1); the block {S2C 224 sub_5EDE00 [me][target][ES id][1471][2][ESClass 1][skill][EState 0], no tail —
// VERIFIED: the client's ESAElectricSwordDo 0x51D790 reads none}; we mark the target (vtbl+132); the target
// stops (sub_41C5D0) and takes the condition 15 (sub_50D650). Its tick (sub_5B7240): the time only — no death
// or caster test (the target gone ends it too); its end (sub_5B72B0): the condition 15 off when on.
void GameClientSession::es_electric_stun(MonsterEntity& m, const SkillDef& d, const EsaRow* act)
{
    uint32_t bits = 0;
    if (monster_debuff_immune(m, d.debuff_bits, bits)) { send_debuff_resisted(m, bits); return; }
    std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
    MonsterEntity::Es e;
    e.id       = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
    e.esaction = 1471;
    e.estate   = 0;
    e.esclass  = 1;
    e.skill    = static_cast<uint16_t>(d.id);
    e.flags    = 2;
    e.caster   = object_id_;
    e.added    = tick_ms();
    e.result   = apply_roll(act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f, 0);
    e.stay     = true;
    e.end_ms   = 1000 * (act ? static_cast<int32_t>(act->get("StunTime", 0.0)) : 0);
    e.life_ms  = e.end_ms;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(static_cast<uint16_t>(e.esaction));
        o.write<uint8_t>(kEsKind);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    m.es.push_back(e);
    mark_monster_owner(m);                                   // vtbl+132(target, 0)
    const auto now = std::chrono::steady_clock::now();
    int32_t x, y;
    m.motion.pos(now, x, y);
    m.motion.rest(x, y);
    m.pos_x = x; m.pos_y = y;
    m.cond_add(15);
    LOG_DEBUG("GameClientSession %u: Lightning Strike stuns mob %u (ES %u, %d ms)", session_id(),
              static_cast<unsigned>(m.object_id), e.id, e.end_ms);
}
