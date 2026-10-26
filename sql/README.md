# ProjectPAULA database scripts

The files `001_*.sql` to `019_*.sql` build the whole game database. Run them in numeric
order, against one database whose name you choose: no file contains `USE` or
`CREATE DATABASE`. Every file can run again safely: tables are created with
`CREATE TABLE IF NOT EXISTS`, columns are added only when they are missing, seed rows use
`INSERT IGNORE`, and nothing is ever dropped. No file creates an account.

## Requirements

- MariaDB 10.2 or newer is the supported server (tested on MariaDB 11.8.9). The servers use
  window functions (`RANK() OVER`), so MySQL 5.7 is too old.
- The servers link MySQL Connector/C 6.1.11 (`libmysql.dll`), which logs in only with
  `mysql_native_password` (it predates MySQL's `caching_sha2_password`). MariaDB needs
  nothing special. MySQL is untested and can work only under these conditions:
  - MySQL 8.0: the servers' account must use `mysql_native_password`.
  - MySQL 8.4: the same, and the plugin must be enabled first (`mysql_native_password=ON`
    in the server's config; it is off by default).
  - MySQL 9.x: removed `mysql_native_password`, so it cannot work.
- The `mysql` / `mariadb` command-line client. Some files use `DELIMITER`, which only the
  command-line client understands (not a GUI's "run SQL file" in most tools).

## New database

1. Edit the settings at the top of `install-db.bat` (in the project root): `MYSQL` (the
   client's path), `DB_HOST`, `DB_PORT`, `DB_USER`, `DB_PASS`, `DB_NAME`. Use the same values
   as `db_host`, `db_port`, `db_user`, `db_pass` and `db_name` in the four `conf\*.conf` files.
   Each setting can also come from an environment variable: `PAULA_MYSQL`, `PAULA_DB_HOST`,
   `PAULA_DB_PORT`, `PAULA_DB_USER`, `PAULA_DB_PASS`, `PAULA_DB_NAME`.
2. Run `install-db.bat`. It creates the database (utf8mb4, `utf8mb4_unicode_ci`) when it is
   missing, then applies every `sql\NNN_*.sql` in order and stops at the first error.
3. Add the accounts: `tools\add-account.bat <username> [gm_level]` (same settings at its
   top). There is no password in v0.1: the account name is the key, so hand out names that
   are hard to guess.
4. To make a character a GM (GM commands, GM-only items), after its player created it in
   the game:
   ```sql
   UPDATE characters SET authority = 1 WHERE name = 'CharacterName';
   ```
   `authority` 0 = player, 1 = GM, 2 = deleted (never set 2 by hand).
   `accounts.gm_level` above 0 only shows the islands that are not open to all.

Without the script, the same steps by hand:

```
mysql -h 127.0.0.1 -P 3307 -u root -p -e "CREATE DATABASE IF NOT EXISTS janus DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
mysql -h 127.0.0.1 -P 3307 -u root -p --default-character-set=utf8mb4 janus < sql\001_login_db.sql
... one line per file, up to sql\019_game_tickets.sql
```

The MariaDB 11 client prints `WARNING: option --ssl-verify-server-cert is disabled, because
of an insecure passwordless login.` when the account has no password. It is harmless; it
goes away once the account has a password.

## Upgrading a database made with janus-emulator or an older ProjectPAULA

1. Stop the servers and back the database up first, for example
   `mysqldump -h 127.0.0.1 -P 3307 -u root -p janus > janus_backup.sql`.
2. Run `install-db.bat` with `DB_NAME` set to that database. It only adds what is missing:
   - `accounts.username` grows from 12 to 18 characters (the client's limit);
   - the new table `game_tickets` (019) is created;
   - any column or table from 001-018 that the database lacks is added.
3. Check the accounts. janus-emulator's `001_login_db.sql` created a default account `admin`
   with `gm_level` 99. Without a password, anyone who types `admin` gets that account.
   Rename it to a name only you know, or delete it if no character uses it:
   ```sql
   SELECT a.account_id, a.username, a.gm_level, COUNT(c.uid) AS characters
     FROM accounts a LEFT JOIN characters c ON c.owner = a.account_id
    GROUP BY a.account_id;
   UPDATE accounts SET username = 'a_secret_name' WHERE username = 'admin';
   ```

What the upgrade leaves alone:
- The janus-era tables that ProjectPAULA never uses are no longer created by a new install,
  but an existing database keeps them, with their data: `angel_pairs`, `cardbattle`,
  `char_online`, `char_quests`, `char_storage`, `couples`, `drill_log`, `friends`,
  `ground_items`, `ip_bans`, `item_db`, `login_log`, `mob_instances`, `parties`,
  `party_members`, `quest_db`, `skill_db`. You may drop them after a backup.
- Existing tables keep their collation. A new install uses `utf8mb4_unicode_ci` everywhere;
  an older database may mix it with the server default (for example `utf8mb4_uca1400_ai_ci`
  on MariaDB 11). This works as it is. To make it uniform, run
  `ALTER TABLE <table> CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;` per
  table. That rebuilds each table, so back up first.

## Database account for the servers

At run time the servers only read and write rows (no DDL), so a dedicated account with DML
rights is enough. Keep MariaDB bound to 127.0.0.1.

```sql
CREATE USER 'paula'@'127.0.0.1' IDENTIFIED BY 'a-strong-password';
GRANT SELECT, INSERT, UPDATE, DELETE ON janus.* TO 'paula'@'127.0.0.1';
```

Then set `db_user` / `db_pass` in the four `conf\*.conf` files. A conf value cannot contain
`#` or `//` (the rest of the line is read as a comment). `install-db.bat` needs an account
that may also create databases, tables and procedures (for example root).

## The files

| File | Creates | Changes |
|---|---|---|
| 001_login_db | accounts, login_tokens | accounts.username to VARCHAR(18) |
| 002_game_characters | characters, char_hack_info | |
| 003_game_items | char_inventory | |
| 004_game_skills_quests | char_skills | |
| 005_world_data | (nothing: world data comes from db/*.yml) | |
| 006_char_status | | characters: sub-stats, TM, hair, job_state, wh_slots |
| 007_item_rare | char_item_option | char_inventory.item_slot |
| 008_quest | char_quest, char_quest_history, char_quest_history_log | |
| 009_warehouse | | char_inventory.item_where (+ index) |
| 010_bank | | char_inventory.user_uid (+ index), accounts.user_money |
| 011_char_create | forbidden (+ the retail name filter rows), mycamp_*, user_mycamp_* (+ the MyCamp starter rows), drill_digup, couple_char, deleted_char | accounts.usable_slot, characters.transgender_date |
| 012_refine | | char_inventory.refine_state, refine_type |
| 013_postbox | post, post_date, post_item | |
| 014_friend | friend_info, friend_group_info, friend_sms | |
| 015_broadcast | broadcast, broadcast_log | |
| 016_memory_port | memory_port | |
| 017_char_names | char_names | |
| 018_party_quest | party_quest, party_quest_member | |
| 019_game_tickets | game_tickets (the LoginServer to GameServer handoff tickets) | |
