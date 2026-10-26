-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — Migration 009: the warehouse
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Mirrors the Paula server's item storage:
--    tbl_item_char.item_where  -> char_inventory.item_where  (1 = the bag — equipped items included,
--                                 they keep their equip_slot —, 2 = the warehouse's first tab,
--                                 21..29 = its other tabs; uspm_item_select_by_where reads one place,
--                                 uspm_item_char_update_move_inven_to_warehouse moves between them)
--    char_state.wh_slots       -> characters.wh_slots        (already there: the warehouse's slot
--                                 count, 80 for a new character, at most 800)
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

CALL paula_add_column('char_inventory', 'item_where', 'TINYINT UNSIGNED NOT NULL DEFAULT 1 AFTER equip_slot');
CALL paula_add_column('characters', 'wh_slots', 'SMALLINT NOT NULL DEFAULT 80');

DROP PROCEDURE IF EXISTS paula_add_column;

DROP PROCEDURE IF EXISTS paula_add_index;

DELIMITER //
CREATE PROCEDURE paula_add_index()
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.STATISTICS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME  = 'char_inventory'
          AND INDEX_NAME  = 'idx_char_where'
    ) THEN
        ALTER TABLE char_inventory ADD INDEX idx_char_where (char_uid, item_where);
    END IF;
END //
DELIMITER ;

CALL paula_add_index();
DROP PROCEDURE IF EXISTS paula_add_index;
