-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — Migration 016: the Memory Port slots (the retail tbl_memory_port)
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Mirrors the Paula server's tbl_memory_port (uspm_memory_port_select / _slot_insert / _slot_delete /
--  _update_move; memory_port.cpp): one saved point of a character's slot list (type 0 = the 5-slot list,
--  1 = the 10-slot one) — its zone, point and description.
--
--  Safe to run more than once.
-- ─────────────────────────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS memory_port (
    char_uid      INT UNSIGNED     NOT NULL,
    type          TINYINT UNSIGNED NOT NULL,
    slot_num      TINYINT UNSIGNED NOT NULL,
    zone          SMALLINT         NOT NULL,
    pos_x         INT              NOT NULL,
    pos_y         INT              NOT NULL,
    pt_desc       VARCHAR(24)      NOT NULL DEFAULT '',
    created_date  DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (char_uid, type, slot_num)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
