-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — Migration 008: quests
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Mirrors the Paula server's quest storage (SQL_Bak trickster_table_empty.sql):
--    tbl_quest          -> char_quest          (the quests in progress; uspm_quest_take inserts,
--                                               uspm_quest_completed / uspm_quest_cancel delete)
--    tbl_quest_history  -> char_quest_history  (how many times a quest was done; uspm_quest_completed
--                                               inserts / updates it, uspm_quest_check_history,
--                                               uspm_select_quest_history_check_count and
--                                               uspm_quest_history_oneday_check read it)
--  quest_type (the SQL numbering, MasterServer dword_591AA4): 1 normal, 2 monster, 3 skill,
--  4 tutorial normal, 5 tutorial monster, 6 scenario, 7 party. The stored procedures are done by the
--  GameServer itself (src/gameserver/quest.cpp), statement by statement.
--
--  Safe to run more than once.
-- ─────────────────────────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS char_quest (
    char_uid        INT UNSIGNED      NOT NULL,
    quest_type      TINYINT UNSIGNED  NOT NULL,
    quest_id        SMALLINT          NOT NULL,
    com_count       TINYINT UNSIGNED  NOT NULL,
    monster_id      SMALLINT          NOT NULL DEFAULT 0,
    kill_count      SMALLINT          NOT NULL DEFAULT 0,
    record_time     INT               NOT NULL DEFAULT 0,
    KEY idx_char (char_uid, quest_type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS char_quest_history (
    char_uid              INT UNSIGNED      NOT NULL,
    quest_type            TINYINT UNSIGNED  NOT NULL,
    quest_id              SMALLINT          NOT NULL,
    com_count             TINYINT UNSIGNED  NOT NULL,
    monster_id            SMALLINT          NOT NULL DEFAULT 0,
    kill_count            SMALLINT          NOT NULL DEFAULT 0,
    record_time           INT               NOT NULL DEFAULT 0,
    quest_completed_date  DATETIME          NOT NULL DEFAULT '2004-01-01 00:00:00',
    PRIMARY KEY (char_uid, quest_id, monster_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- uspm_quest_completed writes a log row for every completion.
CREATE TABLE IF NOT EXISTS char_quest_history_log (
    char_uid              INT UNSIGNED      NOT NULL,
    quest_type            TINYINT UNSIGNED  NOT NULL,
    quest_id              SMALLINT          NOT NULL,
    com_count             TINYINT UNSIGNED  NOT NULL,
    monster_id            SMALLINT          NOT NULL DEFAULT 0,
    kill_count            SMALLINT          NOT NULL DEFAULT 0,
    record_time           INT               NOT NULL DEFAULT 0,
    quest_completed_date  DATETIME          NOT NULL,
    KEY idx_char (char_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
