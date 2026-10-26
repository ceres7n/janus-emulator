-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — Migration 011: character creation (usp_insert_character)
--  Database: the one the installer selects (install-db.bat DB_NAME)
--
--  Mirrors what the Paula server's usp_insert_character writes / reads (trickster_epto, 2025):
--    user_info.usable_slot        -> accounts.usable_slot      (the account's character slots;
--                                    3 on 42,821 of 43,060 Paula accounts, at most 12)
--    char_attr.transgender_date   -> characters.transgender_date ('2000-01-01' at creation)
--    tbl_forbidden                -> forbidden                 (fn_CharacterName_ValidCheck: type 1 =
--                                    the whole name, 2 = anywhere in it, a LIKE pattern, ESCAPE '\';
--                                    the Paula rows below, all type 2)
--    tbl_mycamp_slot / _item / _deco -> mycamp_slot / mycamp_item / mycamp_deco
--                                   (a new character's MyCamp slot 'mycamp 1': one cell 18001 and
--                                    two walls 18002 placed on it)
--    tbl_user_mycamp_item / _log  -> user_mycamp_item / user_mycamp_log
--                                   (usp_user_set_mycamp_item: an account's first character gives the
--                                    account the give_type 1 items + 3 random give_type 2 indexes)
--    tbl_user_mycamp_item_event / _log_event -> user_mycamp_item_event / user_mycamp_log_event
--                                   (usp_user_set_mycamp_item_event: 5 random; empty on Paula)
--    tbl_drill_digup              -> drill_digup               (the drill ranking row, uspm_drill_digup_ins)
--    tbl_couple_char              -> couple_char               (the couple system's row)
--    tbl_deleted_char             -> deleted_char              (uspm_deleted_char_insert: who deleted
--                                    which character, its old name and the client's IP)
--
--  Safe to run more than once.
-- ─────────────────────────────────────────────────────────────────────────────

SET NAMES utf8mb4;

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

CALL paula_add_column('accounts', 'usable_slot', 'TINYINT UNSIGNED NOT NULL DEFAULT 3');
CALL paula_add_column('characters', 'transgender_date',
                      'DATETIME NOT NULL DEFAULT ''2000-01-01 00:00:00'' AFTER job_2_changed');

DROP PROCEDURE IF EXISTS paula_add_column;

CREATE TABLE IF NOT EXISTS forbidden (
    id                SMALLINT UNSIGNED NOT NULL,
    forbidden_type    TINYINT UNSIGNED  NULL,           -- 1 the whole name, 2 anywhere in it
    forbidden_string  VARCHAR(50) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT IGNORE INTO forbidden (id, forbidden_type, forbidden_string) VALUES
    (1, 2, ' '), (2, 2, ''''), (3, 2, '-'), (4, 2, '$'), (5, 2, ','), (6, 2, ';'), (7, 2, '?'), (8, 2, '??'),
    (9, 2, '@'), (10, 2, '\\'), (11, 2, '\\%'), (12, 2, '\\_'), (13, 2, 'admin'), (14, 2, 'Anal'),
    (15, 2, 'bitch'), (16, 2, 'cocksucker'), (17, 2, 'damn'), (18, 2, 'dummy'), (19, 2, 'fuck'),
    (20, 2, 'G?'), (21, 2, 'G_M'), (22, 2, 'gamemaster'), (23, 2, 'gay'), (24, 2, 'GM'), (25, 2, 'GM-'),
    (26, 2, 'Ntreev'), (27, 2, 'Operater'), (28, 2, 'pangya'), (29, 2, 'pennis'), (30, 2, 'porno'),
    (31, 2, 'pussy'), (32, 2, 'sex'), (33, 2, 'sh!t'), (34, 2, 'sh|t'), (35, 2, 'shit'), (36, 2, 'slut'),
    (37, 2, 'trickster'), (38, 2, 'Valofe'), (39, 2, 'viado'), (40, 2, 'Vplay'), (41, 2, 'Vsociety'),
    (42, 2, 'webmaster'), (43, 2, 'whore'), (44, 2, 'กระหรี่'), (45, 2, 'กษัตริย์'), (46, 2, 'กาก'),
    (47, 2, 'กู'), (48, 2, 'เกมมาสเต้อ'), (49, 2, 'เกมมาสเตอร์'), (50, 2, 'เกรียน'), (51, 2, 'ควย'),
    (52, 2, 'แคม'), (53, 2, 'โคตร'), (54, 2, 'เงี่ยน'), (55, 2, 'จัญไร'), (56, 2, 'จิ๋ม'), (57, 2, 'จีเอม'),
    (58, 2, 'จีเอ็ม'), (59, 2, 'จู๋'), (60, 2, 'เจ้าฟ้า'), (61, 2, 'เจ้าอยู่หัว'), (62, 2, 'เจี๊ยว'),
    (63, 2, 'ชิบหาย'), (64, 2, 'ดาก'), (65, 2, 'แดก'), (66, 2, 'ตีน'), (67, 2, 'ทีมงาน'),
    (68, 2, 'บรมวงศ์เธอ'), (69, 2, 'ประมุข'), (70, 2, 'ปิ๊'), (71, 2, 'เปรต'), (72, 2, 'พรรษา'),
    (73, 2, 'พระ'), (74, 2, 'พระนางเจ้า'), (75, 2, 'พระบรม'), (76, 2, 'พระบาท'), (77, 2, 'พระพี่นาง'),
    (78, 2, 'พระสังฆราช'), (79, 2, 'พระองค์'), (80, 2, 'พี่นางเธอ'), (81, 2, 'ภูมิพล'), (82, 2, 'มหาราช'),
    (83, 2, 'มึง'), (84, 2, 'เมิง'), (85, 2, 'แม่ง'), (86, 2, 'แมงดา'), (87, 2, 'แมร่ง'), (88, 2, 'เย็ด'),
    (89, 2, 'รัชกาล'), (90, 2, 'ราชินีนาถ'), (91, 2, 'ลูกยาเธอ'), (92, 2, 'เลว'), (93, 2, 'สถุน'),
    (94, 2, 'สมเด็จ'), (95, 2, 'สัด'), (96, 2, 'สันดาน'), (97, 2, 'เสด็จ'), (98, 2, 'เสือก'),
    (99, 2, 'หม่อมเจ้า'), (100, 2, 'หม๋อย'), (101, 2, 'ห่า'), (102, 2, 'หำ'), (103, 2, 'หี'),
    (104, 2, 'หื่น'), (105, 2, 'เหี้ย'), (106, 2, 'แหก'), (107, 2, 'ไอ้');

CREATE TABLE IF NOT EXISTS mycamp_slot (
    slot_code      INT UNSIGNED NOT NULL AUTO_INCREMENT,
    char_uid       INT UNSIGNED NOT NULL,
    slot_name      VARCHAR(20)  NOT NULL,
    slot_uptodate  DATETIME     NOT NULL,
    PRIMARY KEY (slot_code),
    KEY idx_char (char_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS mycamp_item (
    item_uid       BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
    user_uid       INT UNSIGNED     NOT NULL,           -- the account
    item_index     INT              NOT NULL,           -- a MyCamp item id
    item_where     TINYINT UNSIGNED NOT NULL,           -- 0 kept, 1 placed (mycamp_deco)
    item_buy_date  DATETIME         NOT NULL,
    item_use_date  DATETIME         NULL,
    item_use_time  SMALLINT         NOT NULL DEFAULT 0,
    item_flag      TINYINT UNSIGNED NOT NULL DEFAULT 0,
    sell_code      INT              NOT NULL DEFAULT 0,
    sell_type      TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (item_uid),
    KEY idx_user (user_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS mycamp_deco (
    slot_code       INT UNSIGNED     NOT NULL,
    item_uid        BIGINT UNSIGNED  NOT NULL,
    deco_x          SMALLINT         NOT NULL,
    deco_y          SMALLINT         NOT NULL,
    deco_z          TINYINT UNSIGNED NOT NULL,
    deco_direction  TINYINT UNSIGNED NOT NULL,
    parents_uid     BIGINT UNSIGNED  NOT NULL,
    KEY idx_slot (slot_code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS user_mycamp_item (
    id          SMALLINT UNSIGNED NOT NULL,
    give_type   TINYINT UNSIGNED  NOT NULL,             -- 1 every row, 2 three random indexes
    item_index  INT               NOT NULL,
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT IGNORE INTO user_mycamp_item (id, give_type, item_index) VALUES
    (1, 1, 2000), (2, 1, 3015), (3, 1, 3015), (4, 1, 5002), (5, 1, 8001), (6, 1, 10002), (7, 1, 10015),
    (8, 1, 11000), (9, 1, 15008), (10, 1, 16056), (11, 1, 16056), (12, 1, 16057), (13, 1, 16057),
    (14, 2, 4013), (15, 2, 4013), (16, 2, 6010), (17, 2, 7004), (18, 2, 9007), (19, 2, 12020),
    (20, 2, 15038), (21, 2, 16000), (22, 2, 16041), (23, 2, 16045), (24, 2, 16045), (25, 2, 16045),
    (26, 2, 16045), (27, 2, 17006);

CREATE TABLE IF NOT EXISTS user_mycamp_log (
    user_uid      INT UNSIGNED NOT NULL,
    created_date  DATETIME     NOT NULL,
    PRIMARY KEY (user_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS user_mycamp_item_event (
    item_index  INT NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS user_mycamp_log_event (
    user_uid      INT UNSIGNED NOT NULL,
    created_date  DATETIME     NOT NULL,
    PRIMARY KEY (user_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS drill_digup (
    uid          INT UNSIGNED NOT NULL,                 -- characters.uid
    depth_count  BIGINT       NOT NULL DEFAULT 0,
    dig_count    INT          NOT NULL DEFAULT 0,
    regdate      DATETIME     NOT NULL,
    PRIMARY KEY (uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS couple_char (
    char_uid             INT UNSIGNED NOT NULL,
    couple_code          INT          NOT NULL DEFAULT 0,
    couple_count         SMALLINT     NOT NULL DEFAULT 0,
    couple_delete_count  SMALLINT     NOT NULL DEFAULT 0,
    marriage_count       SMALLINT     NOT NULL DEFAULT 0,
    divorce_count        SMALLINT     NOT NULL DEFAULT 0,
    wedding_in_count     SMALLINT     NOT NULL DEFAULT 0,
    create_date          DATETIME     NOT NULL,
    PRIMARY KEY (char_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS deleted_char (
    user_uid      INT UNSIGNED NOT NULL,
    char_uid      INT UNSIGNED NOT NULL,
    char_name     VARCHAR(17)  NOT NULL,
    user_ip       CHAR(15)     NOT NULL,
    deleted_date  DATETIME     NOT NULL,
    KEY idx_user (user_uid),
    KEY idx_char (char_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- The characters made before this migration: their drill ranking and couple rows (every Paula
-- character has both from usp_insert_character).
INSERT IGNORE INTO drill_digup (uid, depth_count, dig_count, regdate)
    SELECT uid, 0, 0, created_at FROM characters;
INSERT IGNORE INTO couple_char (char_uid, couple_code, couple_count, couple_delete_count, marriage_count,
                                divorce_count, wedding_in_count, create_date)
    SELECT uid, 0, 0, 0, 0, 0, 0, created_at FROM characters;
