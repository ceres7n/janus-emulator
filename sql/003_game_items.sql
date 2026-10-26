-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — 003: inventory
--  Brought in from janus-emulator/sql. Safe to run more than once.
--
--  The janus-era tables item_db, char_storage and ground_items are no longer
--  created: no ProjectPAULA code uses them (item data comes from db/*.yml; the
--  warehouse and bank use char_inventory, see 009/010). An existing database
--  keeps them.
-- ─────────────────────────────────────────────────────────────────────────────

-- ── Character inventory ───────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS char_inventory (
    uid             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    char_uid        INT UNSIGNED    NOT NULL,
    item_id         INT UNSIGNED    NOT NULL,
    amount          SMALLINT UNSIGNED NOT NULL DEFAULT 1,
    -- Inventory slot: 0-x = equip slots, >100 = bag slots
    bag_slot        SMALLINT        NOT NULL DEFAULT -1,   -- -1 = not in bag
    equip_slot      TINYINT         NOT NULL DEFAULT -1,   -- -1 = not equipped
    -- Item instance data (enchants, sockets, etc.)
    durability      SMALLINT UNSIGNED NOT NULL DEFAULT 100,
    refine          TINYINT UNSIGNED NOT NULL DEFAULT 0,
    -- Custom attributes (4 optional slots, like Trickster's item crafting)
    opt1_id         SMALLINT        NOT NULL DEFAULT 0,
    opt1_val        INT             NOT NULL DEFAULT 0,
    opt2_id         SMALLINT        NOT NULL DEFAULT 0,
    opt2_val        INT             NOT NULL DEFAULT 0,
    opt3_id         SMALLINT        NOT NULL DEFAULT 0,
    opt3_val        INT             NOT NULL DEFAULT 0,
    opt4_id         SMALLINT        NOT NULL DEFAULT 0,
    opt4_val        INT             NOT NULL DEFAULT 0,
    obtained_at     DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (uid),
    KEY idx_char (char_uid),
    KEY idx_char_equip (char_uid, equip_slot)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
