-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — 001: login tables (accounts, login_tokens)
--  Brought in from janus-emulator/sql. Applies to the database the installer
--  selects (install-db.bat creates it). Safe to run more than once.
--
--  The janus-era tables login_log and ip_bans are no longer created: no
--  ProjectPAULA code uses them. An existing database keeps them (nothing here
--  drops a table).
-- ─────────────────────────────────────────────────────────────────────────────

-- ── Accounts ──────────────────────────────────────────────────────────────────
-- username = the account name the launcher sends (the client's field is 18
-- characters + NUL; retail user_info.id is varchar(18)).
-- v0.1 does not check a password: password_hash is kept for later
-- (tools\add-account.bat stores an empty string).
CREATE TABLE IF NOT EXISTS accounts (
    account_id      INT UNSIGNED    NOT NULL AUTO_INCREMENT,
    username        VARCHAR(18)     NOT NULL,
    password_hash   VARCHAR(64)     NOT NULL,          -- SHA-256 hex (32 bytes raw)
    email           VARCHAR(255)    NOT NULL DEFAULT '',
    gm_level        TINYINT UNSIGNED NOT NULL DEFAULT 0,
    banned          TINYINT(1)      NOT NULL DEFAULT 0,
    ban_reason      VARCHAR(255)    NOT NULL DEFAULT '',
    ban_until       DATETIME        NULL,
    last_login      DATETIME        NULL,
    last_ip         VARCHAR(45)     NOT NULL DEFAULT '',
    created_at      DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (account_id),
    UNIQUE KEY uq_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Databases made before v0.1 have username VARCHAR(12): widen it to 18.
-- (No change when it already is VARCHAR(18).)
ALTER TABLE accounts MODIFY username VARCHAR(18) NOT NULL;

-- ── Login tokens (AuthServer -> LoginServer handoff) ─────────────────────────
CREATE TABLE IF NOT EXISTS login_tokens (
    token           INT UNSIGNED    NOT NULL,
    account_id      INT UNSIGNED    NOT NULL,
    gm_level        TINYINT UNSIGNED NOT NULL DEFAULT 0,
    expires_at      DATETIME        NOT NULL,
    PRIMARY KEY (token),
    KEY idx_account (account_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- No default account: the operator adds accounts with tools\add-account.bat.
