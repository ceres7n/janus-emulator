-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — 019: LoginServer -> GameServer handoff tickets
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Not a retail table: a v0.1 hardening of the handoff to the GameServer.
--  On a character select the LoginServer checks that the character belongs to
--  the session's account, then writes (REPLACE) one row for it: the account,
--  the client's IP and an expiry 300 seconds ahead. The GameServer accepts the
--  first enter (C2S 0x0004) and a re-enter (C2S 156) only with an unexpired row
--  for that character, from the same IP, whose account owns the character, and
--  deletes the row once it is used. Before every reconnect it orders itself
--  (S2C 154 RELOCATE) it writes a fresh row the same way.
--  char_uid / account_id have the types of characters.uid / accounts.account_id.
--
--  Safe to run more than once.
-- ─────────────────────────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS game_tickets (
    char_uid        INT UNSIGNED    NOT NULL,          -- characters.uid
    account_id      INT UNSIGNED    NOT NULL,          -- accounts.account_id (the owner)
    client_ip       VARCHAR(45)     NOT NULL,          -- the client's IP, as the server saw it
    expires_at      DATETIME        NOT NULL,
    PRIMARY KEY (char_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
