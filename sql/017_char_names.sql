-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — Migration 017: the character name changes (the retail tbl_char_names)
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Mirrors the Paula server's tbl_char_names (uspm_insert_char_names_changed: one row per rename by an
--  item — the uid, the old and new names, when, why — "by item using(<item index>)" — and the old name
--  again; CharStore::RenameCharacter).
--
--  Safe to run more than once.
-- ─────────────────────────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS char_names (
    seq          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    char_uid     INT UNSIGNED    NOT NULL,
    old_name     VARCHAR(17)     NOT NULL,
    new_name     VARCHAR(17)     NOT NULL,
    change_date  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    reason       VARCHAR(100)    NOT NULL DEFAULT '',
    first_name   VARCHAR(17)     NOT NULL DEFAULT '',
    PRIMARY KEY (seq),
    KEY ix_char (char_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
