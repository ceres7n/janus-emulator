-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — Migration 014: the friend list (the retail SystemServer's FriendServer)
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Mirrors the Paula server's friend tables (usp_select_friend_info, usp_insert_friend_info,
--  usp_update_friend_info_set_delete, usp_friend_group_create, usp_select_friend_sms ...;
--  friend_server.cpp):
--    friend_info        one row per side of a friendship (ownerid's list holds friendid, in its
--                       group 0..4); deleted = 1: the other side dropped the friendship while this
--                       owner was offline (shown and removed at its next login)
--    friend_group_info  the five group names of a character (made at its first friend login)
--    friend_sms         the messenger's offline notes (read and removed at login)
--
--  Safe to run more than once.
-- ─────────────────────────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS friend_info (
    ownerid   INT UNSIGNED     NOT NULL,
    friendid  INT UNSIGNED     NOT NULL,
    groupNum  TINYINT UNSIGNED NOT NULL DEFAULT 0,
    deleted   TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (ownerid, friendid),
    KEY ix_friend (friendid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS friend_group_info (
    ownerid   INT UNSIGNED     NOT NULL,
    groupNum  TINYINT UNSIGNED NOT NULL,
    groupName VARCHAR(20)      NOT NULL DEFAULT '',
    PRIMARY KEY (ownerid, groupNum)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS friend_sms (
    sms_uid   INT UNSIGNED     NOT NULL AUTO_INCREMENT,
    ownerid   INT UNSIGNED     NOT NULL,
    sender    INT UNSIGNED     NOT NULL,
    dtime     DATETIME         NOT NULL,
    content   VARCHAR(255)     NOT NULL DEFAULT '',
    sms_kind  TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (sms_uid),
    KEY ix_owner (ownerid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
