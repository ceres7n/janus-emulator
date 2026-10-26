#pragma once
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
//  Trickster Online — Game Constants (named enums)
//
//  Source of truth for the values used across the db/*.yml data files. Each YAML
//  value is the exact enumerator name below (rAthena-style named constants, no
//  magic numbers). The data loader maps the string to the enum and rejects any
//  unknown name on load.
//
//  Sources: KR DB trickster_epto (tbl_job_s_type / tbl_job_f_type / tbl_job_info),
//  client JobTypeInfo.xml and MapInfoEx.xml.
// ─────────────────────────────────────────────────────────────────────────────

// Character race / chrtype (characters.type). 1-8 from tbl_job_s_type; 9 = Bear
// (KR-2014 addition, client-only). Used by: races_db.yml (Id), jobs_db.yml (Race).
enum class Race : uint8_t
{
    Bunny   = 1,
    Buffalo = 2,
    Sheep   = 3,
    Dragon  = 4,
    Fox     = 5,
    Lion    = 6,
    Cat     = 7,
    Raccoon = 8,
    Bear    = 9,
};

// Stat growth type (f_type). The My View "Growth" order Power/Magic/Sense/Charm
// = characters.pts_p/pts_m/pts_s/pts_c. (DB labels type 0 "Attack".)
// Used by: state_types_db.yml (Id), races_db.yml / jobs_db.yml (StateType).
enum class StateType : uint8_t
{
    Power = 0,
    Magic = 1,
    Sense = 2,
    Charm = 3,
};

// Job advancement tier. Used by: jobs_db.yml (Degree).
enum class JobDegree : uint8_t
{
    Base   = 0,   // starting class
    First  = 1,   // 1st job change
    Second = 2,   // 2nd job change
};

// Character class / JobType (client JobTypeInfo.xml numbering — the bear is woven
// into every tier). Used by: jobs_db.yml (Id), races_db.yml (BaseJob).
enum class Job : uint8_t
{
    // degree 0 — base (one per race)
    SchoolGirl    = 1,
    Fighter       = 2,
    Librarian     = 3,
    Shaman        = 4,
    Archaeologist = 5,
    Engineer      = 6,
    Model         = 7,
    Teacher       = 8,
    AnimalLover   = 9,    // Bear
    // degree 1 — 1st job (one per race)
    Boxer         = 10,
    Warrior       = 11,
    Bard          = 12,
    Magician      = 13,
    Explorer      = 14,
    Inventor      = 15,
    Talent        = 16,
    CardMaster    = 17,
    Trainer       = 18,   // Bear
    // degree 2 — 2nd job (two per race; Dragon three; Bear one)
    Champion      = 19,
    Duelist       = 20,
    Mercenary     = 21,
    Gladiator     = 22,
    SoulMaster    = 23,
    Witch         = 24,
    Wizard        = 25,
    DarkLord      = 26,
    Priest        = 27,
    ThiefMaster   = 28,
    HunterLord    = 29,
    CyberHunter   = 30,
    Scientist     = 31,
    Primadonna    = 32,
    Diva          = 33,
    Duke          = 34,
    Gambler       = 35,
    Zoologist     = 36,   // Bear
};

// Map/field type (MapInfoEx.xml "Type"). Used by: maps_db.yml (Type).
// Values 10-11 are unused by the current dataset. 13/15 are provisional
// (Korean-only map names, exact purpose unconfirmed).
enum class MapType : uint8_t
{
    Intro        = 0,
    TownPlaza    = 1,
    Gate         = 2,    // entrance between regions
    Path         = 3,    // outdoor field
    Dungeon      = 4,
    Indoor       = 5,
    MiniGame     = 6,    // OX quiz / event gardens
    Battlefield  = 7,    // PvP / event arenas
    SoccerArena  = 8,
    DuelField    = 9,
    Hall         = 12,   // Megalo/Caballa Hall
    SpecialZone  = 13,   // provisional
    BossRoom     = 14,   // Tapasco boss/puzzle rooms
    UnknownSpace = 15,   // provisional ("미지의 공간")
};
