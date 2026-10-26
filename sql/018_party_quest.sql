-- ─────────────────────────────────────────────────────────────────────────────
--  018_party_quest.sql — the party quests (party_quest.cpp).
--
--  The Paula procedures' tables (DBReader\procs_paula): PartyQuest (USPM_Get_PartyQuestUid inserts a row per
--  accepted quest: QuestId, MonsterId, TargetCount, RecordTime, MemberCount, the date; USPM_Complete_PartyQuest
--  sets RecordTime / MemberCount) and PartyQuestMember (USPM_Complete_PartyQuest: a row per member who finished
--  it — level, job, its kills, the times done; USPM_Reward_PartyQuest sets RewardDate). The quest's history uses
--  char_quest / char_quest_history / char_quest_history_log (sql/008) with quest_type 7.
--
--  Safe to run more than once.
-- ─────────────────────────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS party_quest (
    party_uid       BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT,   -- PartyUid (@@IDENTITY)
    quest_id        SMALLINT          NOT NULL,
    monster_id      SMALLINT          NOT NULL,
    target_count    SMALLINT          NOT NULL,
    record_time     INT               NOT NULL DEFAULT 0,
    member_count    TINYINT UNSIGNED  NOT NULL DEFAULT 0,
    reg_date        DATETIME          NOT NULL,
    PRIMARY KEY (party_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS party_quest_member (
    party_uid       BIGINT UNSIGNED   NOT NULL,
    char_uid        INT UNSIGNED      NOT NULL,
    level           SMALLINT          NOT NULL,
    char_job        INT               NOT NULL,
    kill_count      SMALLINT          NOT NULL,
    com_count       INT               NOT NULL,
    reward_date     DATETIME          NULL,
    KEY idx_char (char_uid, reward_date),
    KEY idx_party (party_uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
