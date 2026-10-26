-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — Migration 007: rare item data (compound slots + options)
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Mirrors the Paula server's item storage:
--    tbl_item.item_slot        -> char_inventory.item_slot  (a rare item's compound slot count,
--                                 byte 3 of its 4 rare bytes on the wire; sub_6EC2B0)
--    tbl_item_option           -> char_item_option          (usp_item_option_insert:
--                                 option_type 0~9 compound slots, 10 base (+random) stats,
--                                 11 refine, 12~ others; one row per (option_type, option_index))
--  A dropped rare item keeps its row with char_uid = 0 while it lies on the ground
--  (uspm_item_update_uid_item_rare drop_flag 0) and gets the picker's char_uid back on pick-up
--  (drop_flag 1); deleting an item deletes its options too (uspm_item_delete_item).
--
--  Safe to run more than once.
-- ─────────────────────────────────────────────────────────────────────────────

DROP PROCEDURE IF EXISTS paula_add_column;

DELIMITER //
CREATE PROCEDURE paula_add_column(
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
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
    END IF;
END //
DELIMITER ;

CALL paula_add_column('char_inventory', 'item_slot', 'TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER refine');

DROP PROCEDURE IF EXISTS paula_add_column;

CREATE TABLE IF NOT EXISTS char_item_option (
    item_uid        BIGINT UNSIGNED  NOT NULL,          -- char_inventory.uid
    option_type     TINYINT UNSIGNED NOT NULL,          -- 0~9 compound slot, 10 base/random stat, 11 refine
    option_index    SMALLINT         NOT NULL,          -- the option id (10000..10012 stats, 24000/24001 ...)
    option_value    SMALLINT         NOT NULL,
    mixed_item      INT              NOT NULL DEFAULT 0,
    PRIMARY KEY (item_uid, option_type, option_index)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
