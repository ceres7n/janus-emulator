-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — Migration 010: the NeoWarehouse (the account's bank)
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Mirrors the Paula server's bank storage:
--    tbl_item_user (user_uid, item_uid, item_where 3) -> char_inventory rows with item_where 3,
--                                 char_uid 0 and user_uid = the account (uspm_item_select_by_where
--                                 3: the account's first 24 by item uid; uspm_item_char_update_move_
--                                 inven_to_bank moves between the bag and the bank)
--    user_info.user_money      -> accounts.user_money (the bank's gelder; uspm_user_info_update_
--                                 move_money)
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

CALL paula_add_column('char_inventory', 'user_uid', 'INT UNSIGNED NOT NULL DEFAULT 0 AFTER char_uid');
CALL paula_add_column('accounts', 'user_money', 'INT NOT NULL DEFAULT 0');

DROP PROCEDURE IF EXISTS paula_add_column;

DROP PROCEDURE IF EXISTS paula_add_index;

DELIMITER //
CREATE PROCEDURE paula_add_index()
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.STATISTICS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME  = 'char_inventory'
          AND INDEX_NAME  = 'idx_user_where'
    ) THEN
        ALTER TABLE char_inventory ADD INDEX idx_user_where (user_uid, item_where);
    END IF;
END //
DELIMITER ;

CALL paula_add_index();
DROP PROCEDURE IF EXISTS paula_add_index;
