-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — Migration 013: the post box (PostBoxUI: mail, gifts, level-up rewards)
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Mirrors the Paula server's post tables (the USPM_*Post* / *Gift* procedures, post_store.cpp):
--    Post      -> post       one mail: PostType 0 the post box / 1 the gift box; PostSubType 0 normal,
--                            1 gift (items / gelders, the sender pays), 2 trade (gelders requested),
--                            3 the trade's payment, 4 paid gift, 5 GM gift; PostState 0 unread,
--                            1 read, 2 returned, 3 deleted
--    PostDate  -> post_date  its history: every state change adds a row per side (UserType 0 the
--                            sender, 1 the receiver); a list shows each post's latest row (RANK by
--                            DateSeq) — DateSeq is the client's paging / sync key
--    PostItem  -> post_item  its attached items (a common stack is kept here by index + count; a
--                            rare item keeps its char_inventory row, char_uid 0 while it travels)
--
--  Safe to run more than once.
-- ─────────────────────────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS post (
    post_uid             INT UNSIGNED     NOT NULL AUTO_INCREMENT,
    post_type            TINYINT UNSIGNED NOT NULL DEFAULT 0,
    post_sub_type        TINYINT UNSIGNED NOT NULL DEFAULT 0,
    post_state           TINYINT UNSIGNED NOT NULL DEFAULT 0,
    title                VARCHAR(50)      NOT NULL DEFAULT '',
    content              VARCHAR(300)     NOT NULL DEFAULT '',
    post_money_tax       INT              NOT NULL DEFAULT 0,
    post_money           INT              NOT NULL DEFAULT 0,
    post_money_take_date DATETIME         NULL,
    sell_code            INT              NOT NULL DEFAULT 0,
    parent_post_uid      INT UNSIGNED     NOT NULL DEFAULT 0,
    create_date          DATETIME         NOT NULL,
    PRIMARY KEY (post_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS post_date (
    date_seq    BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
    post_uid    INT UNSIGNED     NOT NULL,
    post_state  TINYINT UNSIGNED NOT NULL,
    user_type   TINYINT UNSIGNED NOT NULL,
    user_uid    INT UNSIGNED     NOT NULL,
    char_uid    INT UNSIGNED     NOT NULL,
    char_name   VARCHAR(17)      NOT NULL DEFAULT '',
    char_level  SMALLINT         NOT NULL DEFAULT 0,
    send_date   DATETIME         NOT NULL,
    return_date DATETIME         NOT NULL,
    create_date DATETIME         NOT NULL,
    PRIMARY KEY (date_seq),
    KEY ix_post_state (post_uid, post_state),
    KEY ix_user_char (user_uid, char_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS post_item (
    item_seq        BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
    post_uid        INT UNSIGNED     NOT NULL,
    item_uid        BIGINT UNSIGNED  NOT NULL DEFAULT 0,
    item_class      TINYINT UNSIGNED NOT NULL,
    item_index      INT UNSIGNED     NOT NULL,
    item_attr       SMALLINT         NOT NULL,
    add_minute      INT              NOT NULL DEFAULT 0,
    item_flag       TINYINT UNSIGNED NOT NULL DEFAULT 0,
    create_date     DATETIME         NOT NULL,
    take_char_uid   INT UNSIGNED     NULL,
    take_char_level SMALLINT         NULL,
    take_date       DATETIME         NULL,
    PRIMARY KEY (item_seq),
    KEY ix_post (post_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
