-- ─────────────────────────────────────────────────────────────────────────────
--  ProjectPAULA — 002: characters
--  Brought in from janus-emulator/sql. Source: char_attr + char_state from
--  old_mssql_db_documentation.json. Safe to run more than once.
--
--  The janus-era tables char_online and cardbattle are no longer created: no
--  ProjectPAULA code uses them. An existing database keeps them.
-- ─────────────────────────────────────────────────────────────────────────────

-- ── Characters ───────────────────────────────────────────────────────────────
-- Merges original char_attr (character definition) + char_state (game state)
CREATE TABLE IF NOT EXISTS characters (
    uid             INT UNSIGNED    NOT NULL AUTO_INCREMENT,
    owner           INT UNSIGNED    NOT NULL,           -- account_id
    slot            TINYINT UNSIGNED NOT NULL DEFAULT 0, -- 0-2

    -- Identification (from char_attr.name VARCHAR(17))
    name            VARCHAR(17)     NOT NULL,

    -- Type/Job system (original: type SMALLINT, job SMALLINT)
    -- Trickster base types: 1=Fox, 2=Sheep, 3=Cat, 4=Dragon, 5=Bunny, ...
    type            SMALLINT        NOT NULL DEFAULT 1,
    job             SMALLINT        NOT NULL DEFAULT 0,
    f_type          TINYINT UNSIGNED NOT NULL DEFAULT 0, -- appearance/gender

    -- Second job (original: type_2, job_2, job_2_changed)
    type_2          SMALLINT        NOT NULL DEFAULT 0,
    job_2           TINYINT UNSIGNED NOT NULL DEFAULT 0,
    job_2_changed   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

    -- Third job (original: type_3, job_3, job_3_changed)
    type_3          SMALLINT        NOT NULL DEFAULT 0,
    job_3           TINYINT UNSIGNED NOT NULL DEFAULT 0,
    job_3_changed   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

    -- Job change quest state (original: change_job_queststate TINYINT)
    change_job_queststate TINYINT UNSIGNED NOT NULL DEFAULT 0,

    -- PMSC base stat allocation points (from char_attr: pts_p/m/s/c TINYINT)
    pts_p           TINYINT UNSIGNED NOT NULL DEFAULT 0, -- Power
    pts_m           TINYINT UNSIGNED NOT NULL DEFAULT 0, -- Magic
    pts_s           TINYINT UNSIGNED NOT NULL DEFAULT 0, -- Sense
    pts_c           TINYINT UNSIGNED NOT NULL DEFAULT 0, -- Charm

    -- Combat stats (computed from pts + equipment + level, stored for fast load)
    level           SMALLINT UNSIGNED NOT NULL DEFAULT 1,
    exp             BIGINT UNSIGNED NOT NULL DEFAULT 0,
    hp              INT             NOT NULL DEFAULT 100,
    hp_max          INT             NOT NULL DEFAULT 100,
    mp              INT             NOT NULL DEFAULT 100,
    mp_max          INT             NOT NULL DEFAULT 100,

    -- Game state (from char_state)
    state           TINYINT UNSIGNED NOT NULL DEFAULT 0,
    -- 0 = player, 1 = GM (GM commands, GM-only items), 2 = deleted (hidden from the list)
    authority       TINYINT UNSIGNED NOT NULL DEFAULT 0,
    zone            SMALLINT        NOT NULL DEFAULT 1,
    pos_x           INT             NOT NULL DEFAULT 100,
    pos_y           INT             NOT NULL DEFAULT 100,
    home            SMALLINT        NOT NULL DEFAULT 1,
    money           INT             NOT NULL DEFAULT 0,

    -- Meta / deletion
    -- deleted=0: active | deleted=1: frozen (pending deletion) | deleted=2: purged
    deleted         TINYINT UNSIGNED NOT NULL DEFAULT 0,
    freeze_until    DATETIME        NULL,    -- NULL = not frozen; deletion completes after this date
    created_at      DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_login      DATETIME        NULL,

    PRIMARY KEY (uid),
    UNIQUE KEY uq_name (name),
    KEY idx_owner (owner),
    KEY idx_owner_slot (owner, slot)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ── Hack info (from original char_hack_info) ─────────────────────────────────
-- Tracks anti-cheat flags
CREATE TABLE IF NOT EXISTS char_hack_info (
    char_uid        INT UNSIGNED    NOT NULL,
    hack_index      TINYINT UNSIGNED NOT NULL DEFAULT 0,
    hack_created    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (char_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
