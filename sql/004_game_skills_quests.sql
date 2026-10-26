-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — 004: learned skills
--  Brought in from janus-emulator/sql. Safe to run more than once.
--
--  The janus-era tables skill_db, quest_db, char_quests, parties,
--  party_members, friends, couples, angel_pairs and drill_log are no longer
--  created: no ProjectPAULA code uses them (skill and quest data come from
--  db/*.yml; quests use 008, friends 014, couples 011, party quests 018). An
--  existing database keeps them.
-- ─────────────────────────────────────────────────────────────────────────────

-- ── Character skills ─────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS char_skills (
    char_uid        INT UNSIGNED    NOT NULL,
    skill_id        INT UNSIGNED    NOT NULL,
    level           TINYINT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (char_uid, skill_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
