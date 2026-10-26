-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — 006: char_status sub-stats + missing char_state fields
--  Brought in from janus-emulator/sql. Source: char_status + char_state from
--  old_mssql_db_documentation.json
--
--  char_status  = secondary combat stats allocated from pts_p/m/s/c
--  char_state   = runtime state (hair, job_state, wh_slots, TM exp per stat)
--
--  MySQL 8.x has no ADD COLUMN IF NOT EXISTS, so every column goes through the
--  janus_add_column procedure below, which skips a column that already exists:
--  running this file again changes nothing. Needs the mysql/mariadb command-line
--  client (DELIMITER).
-- ─────────────────────────────────────────────────────────────────────────────

-- Use a stored procedure to add each column only when it doesn't exist yet.
DROP PROCEDURE IF EXISTS janus_add_column;

DELIMITER //
CREATE PROCEDURE janus_add_column(
    IN p_table  VARCHAR(64),
    IN p_col    VARCHAR(64),
    IN p_def    TEXT
)
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME  = p_table
          AND COLUMN_NAME = p_col
    ) THEN
        SET @sql = CONCAT('ALTER TABLE `', p_table, '` ADD COLUMN `', p_col, '` ', p_def);
        PREPARE s FROM @sql;
        EXECUTE s;
        DEALLOCATE PREPARE s;
    END IF;
END //
DELIMITER ;

-- ── Sub-stats from char_status ────────────────────────────────────────────────
CALL janus_add_column('characters', 'ap_pts',        'SMALLINT NOT NULL DEFAULT 0 COMMENT "Attack Power"');
CALL janus_add_column('characters', 'ac_pts',        'SMALLINT NOT NULL DEFAULT 0 COMMENT "Accuracy"');
CALL janus_add_column('characters', 'dx_pts',        'SMALLINT NOT NULL DEFAULT 0 COMMENT "Dexterity"');
CALL janus_add_column('characters', 'max_mp_pts',    'SMALLINT NOT NULL DEFAULT 0 COMMENT "Max MP stat pts"');
CALL janus_add_column('characters', 'ma_pts',        'SMALLINT NOT NULL DEFAULT 0 COMMENT "Magic Attack"');
CALL janus_add_column('characters', 'md_pts',        'SMALLINT NOT NULL DEFAULT 0 COMMENT "Magic Defense"');
CALL janus_add_column('characters', 'max_wt_pts',    'SMALLINT NOT NULL DEFAULT 0 COMMENT "Max Weight stat pts"');
CALL janus_add_column('characters', 'da_pts',        'SMALLINT NOT NULL DEFAULT 0 COMMENT "Dodge/Avoidance"');
CALL janus_add_column('characters', 'lk_pts',        'SMALLINT NOT NULL DEFAULT 0 COMMENT "Luck"');
CALL janus_add_column('characters', 'max_hp_pts',    'SMALLINT NOT NULL DEFAULT 0 COMMENT "Max HP stat pts"');
CALL janus_add_column('characters', 'dp_pts',        'SMALLINT NOT NULL DEFAULT 0 COMMENT "Defense"');
CALL janus_add_column('characters', 'hv_pts',        'SMALLINT NOT NULL DEFAULT 0 COMMENT "Evasion"');
CALL janus_add_column('characters', 'attr_pts',      'SMALLINT NOT NULL DEFAULT 0 COMMENT "Unspent attribute points"');
-- TM (Trickster Mission) levels per base stat
CALL janus_add_column('characters', 'tm_level_p',    'SMALLINT NOT NULL DEFAULT 0');
CALL janus_add_column('characters', 'tm_level_m',    'SMALLINT NOT NULL DEFAULT 0');
CALL janus_add_column('characters', 'tm_level_s',    'SMALLINT NOT NULL DEFAULT 0');
CALL janus_add_column('characters', 'tm_level_c',    'SMALLINT NOT NULL DEFAULT 0');
-- TM overall stats
CALL janus_add_column('characters', 'tm_point',      'SMALLINT NOT NULL DEFAULT 0');
CALL janus_add_column('characters', 'tm_level',      'SMALLINT NOT NULL DEFAULT 0');
CALL janus_add_column('characters', 'tm_exp',        'INT      NOT NULL DEFAULT 0');
CALL janus_add_column('characters', 'tutorial_step', 'TINYINT  NOT NULL DEFAULT 0');

-- ── Missing char_state fields ─────────────────────────────────────────────────
-- Hair style / colour / extra layers
CALL janus_add_column('characters', 'hair',          'TINYINT  NOT NULL DEFAULT 0');
CALL janus_add_column('characters', 'hair_2',        'TINYINT  NOT NULL DEFAULT 0');
CALL janus_add_column('characters', 'hair_3',        'TINYINT  NOT NULL DEFAULT 0');
-- Job advancement state (1 = first job, 2 = second, etc.)
CALL janus_add_column('characters', 'job_state',     'TINYINT  NOT NULL DEFAULT 1');
-- Warehouse slot count (original default was 80)
CALL janus_add_column('characters', 'wh_slots',      'SMALLINT NOT NULL DEFAULT 80');
-- TM exp accumulated per base stat
CALL janus_add_column('characters', 'tm_exp_p',      'INT      NOT NULL DEFAULT 0');
CALL janus_add_column('characters', 'tm_exp_m',      'INT      NOT NULL DEFAULT 0');
CALL janus_add_column('characters', 'tm_exp_s',      'INT      NOT NULL DEFAULT 0');
CALL janus_add_column('characters', 'tm_exp_c',      'INT      NOT NULL DEFAULT 0');

DROP PROCEDURE IF EXISTS janus_add_column;
