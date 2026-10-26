-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — Migration 012: item refine state (Item_Refine / Repair, state 30)
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Mirrors the Paula server's tbl_item refine columns:
--    tbl_item.refine_level -> char_inventory.refine        (already there; byte +49 on the wire)
--    tbl_item.refine_state -> char_inventory.refine_state  (0 ok, 1 failed — the item refuses to
--                             equip until a Repair Powder fixes it; byte +50)
--    tbl_item.refine_type  -> char_inventory.refine_type   (the refined option index, 10000..10012;
--                             uspm_item_update_refine_success)
--  The refined bonus itself is a char_item_option row of option_type 11 (the same table the
--  mature compound's slot options 0..9 live in).
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

CALL paula_add_column('char_inventory', 'refine_state', 'TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER refine');
CALL paula_add_column('char_inventory', 'refine_type',  'SMALLINT NOT NULL DEFAULT 0 AFTER refine_state');

DROP PROCEDURE IF EXISTS paula_add_column;
