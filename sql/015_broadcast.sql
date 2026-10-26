-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — Migration 015: the megaphone queue (the retail Master's broadcast messages)
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Mirrors the Paula server's tbl_broadcast / tbl_broadcast_log (uspm_insert_broadcast_message,
--  uspm_select_broadcast_message, uspm_update_broadcast_success; game_server.cpp megaphone_*):
--    broadcast      one queued message; message_flag 0 a GM's, 1 registered (a player's), 2 broadcast,
--                   3 given back as the item, 4 not given back (bag full); broadcast_type 0 GM, 1 normal
--    broadcast_log  the broadcasts done (message_flag 2)
--
--  Safe to run more than once.
-- ─────────────────────────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS broadcast (
    broadcast_code  BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
    char_uid        INT UNSIGNED     NOT NULL DEFAULT 0,
    island_code     TINYINT UNSIGNED NOT NULL DEFAULT 0,
    message         VARCHAR(300)     NOT NULL DEFAULT '',
    message_flag    TINYINT UNSIGNED NOT NULL DEFAULT 1,
    broadcast_type  TINYINT UNSIGNED NOT NULL DEFAULT 1,
    color_type      TINYINT UNSIGNED NOT NULL DEFAULT 0,
    created_date    DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (broadcast_code),
    KEY ix_flag (message_flag)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS broadcast_log (
    log_seq         BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
    broadcast_code  BIGINT UNSIGNED  NOT NULL,
    message_flag    TINYINT UNSIGNED NOT NULL,
    created_date    DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (log_seq),
    KEY ix_code (broadcast_code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
