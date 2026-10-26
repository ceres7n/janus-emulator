# Configuration reference

Each server reads one text file from `conf\`:

| Server | File |
|---|---|
| AuthServer | `conf\auth_server.conf` |
| LoginServer | `conf\login_server.conf` |
| MasterServer | `conf\master_server.conf` |
| GameServer | `conf\game_server.conf` |

A server reads its file once, at start. Restart the server after a change.

For a normal install only a few keys matter. They are listed in
[INSTALL.md](INSTALL.md#7-configure-the-servers). This page lists every key.

## File syntax

```
// A comment
key: value
db_port: 3307   // a comment after a value
```

- One `key: value` per line. The first `:` separates the key from the value, so a value may
  contain `:` (for example a Windows path). Spaces around the key and the value are ignored.
- **`//` and `#` start a comment anywhere on a line**, even inside a value. A value therefore
  **cannot contain `#` or `//`**: a password such as `abc#123` is read as `abc`. There is no
  quoting or escaping.
- Yes/no keys accept `yes`, `true`, `1` and `no`, `false`, `0`. Any other text means the default.
- Numbers must be plain integers. A value that is not a number means the default.
- If a key appears twice, the last one wins. Unknown keys are ignored silently, so check the
  spelling. A line without `:` gives a warning in the log.
- Line endings may be CRLF or LF.

Paths:
- Each server makes the folder of its `.exe` its working folder at start. Relative paths in the
  conf files (`db/...`, `data/map`, `tools/...`) are relative to that folder.
- A server takes the path of another conf file as its first argument, for example
  `GameServer.exe conf\test_game.conf`. A relative path there is relative to the folder you start
  it from.
- If the conf file is missing, the server logs a warning and runs on the defaults below.

## Keys every server reads

| Key | Default | Meaning |
|---|---|---|
| `log_level` | `info` | Log detail: `debug`, `info`, `warn` or `error`. `debug` logs every packet: large and slow. An unknown value means `info`. See [INSTALL.md](INSTALL.md#11-logs-and-crash-dumps). |
| `io_threads` | `2` (GameServer: `1`) | Network threads. The v0.1 GameServer always uses exactly 1 and ignores other values with a warning. |
| `db_host` | `127.0.0.1` | MariaDB host |
| `db_port` | `3307` | MariaDB port (`3307` is WampServer's MariaDB; a standalone MariaDB uses `3306`) |
| `db_user` | `root` | MariaDB user. Use a user with `SELECT, INSERT, UPDATE, DELETE` only ([INSTALL.md](INSTALL.md#6-give-the-servers-their-own-database-user)). |
| `db_pass` | empty | That user's password (no `#` or `//`) |
| `db_name` | `janus` | Database name |

`db_*` must be set in all four files; there is no shared include file.

## AuthServer (`conf\auth_server.conf`)

The client connects here first. The AuthServer looks up the account, refuses unknown or banned
accounts, and sends the island list.

| Key | Default | Meaning |
|---|---|---|
| `port` | `9980` | Listen port. **Keep 9980**: the client always connects to 9980. |
| `bind_ip` | `0.0.0.0` | The IPv4 address to listen on. `0.0.0.0` = every network interface. |
| `paula_mode` | `true` | The KR 2014 client protocol. `false` is not supported: the AuthServer refuses to start. |
| `sbox_path` | `tools/capture/sbox_cora.bin` | The client's packet cipher table (65,536 bytes) |
| `char_server_ip` | `127.0.0.1` | LoginServer address for the default island. **Only used when there is no `island_N` line.** |
| `char_server_port` | `8001` | LoginServer port for the default island. Same condition. |
| `island_1` ... `island_50` | none | The island list (see below). Without any `island_N` line, the server lists one island, "Ruby Island" in the world "Paula", at `char_server_ip:char_server_port`. |

### Island lines

```
island_N: world, island no, world name, island name, ip, port[, open[, open to all]]
```

| Field | Meaning |
|---|---|
| `world` | World number. A line with world `0` is ignored. |
| `island no` | Island number within the world |
| `world name` | Shown as the window title "Trickster <world name>" after connecting, and in TBN megaphone lines |
| `island name` | The text of the island button |
| `ip` | The LoginServer address the client connects to when this island is chosen. **The address players reach**: a numeric IPv4 address, at most 15 characters. |
| `port` | That LoginServer's port (`8001`) |
| `open` | Optional, default 1. `0` = listed, but choosing it is refused. |
| `open to all` | Optional, default 1. `0` = listed only for accounts with `gm_level` > 0. |

Notes:
- The shipped line is `island_1: 1, 1, Paula, Ruby Island, 127.0.0.1, 8001`. The original
  Eternal Paula server had one island, "Ruby Island".
- Fields are separated by commas, so names cannot contain commas. Names are converted to the
  Korean code page (CP949) and cut to 32 bytes.
- The client shows at most 6 islands.
- **With exactly 2 lines** the client opens its old two-world window, which shows no names. Use
  one line, or three or more.

## LoginServer (`conf\login_server.conf`)

Character select. The client arrives here with the token the AuthServer issued.

| Key | Default | Meaning |
|---|---|---|
| `port` | `8001` | Listen port. Must match the `port` field of the island lines. |
| `bind_ip` | `0.0.0.0` | The IPv4 address to listen on |
| `paula_mode` | `true` | Keep `true` |
| `sbox_path` | `tools/capture/sbox_cora.bin` | The client's packet cipher table |
| `game_server_ip` | `127.0.0.1` | **The GameServer address sent to the client** after the character select: the address players reach. The LoginServer logs a warning while it is a loopback address. |
| `game_server_port` | `8002` | GameServer port sent to the client. Must equal the GameServer's `game_port`. |
| `master_ip` | `127.0.0.1` | MasterServer address. Keep `127.0.0.1`. |
| `master_port` | `9010` | MasterServer port |

## MasterServer (`conf\master_server.conf`)

The character data service the LoginServer uses (character list, creation, deletion). Players
never connect to it.

| Key | Default | Meaning |
|---|---|---|
| `master_port` | `9010` | Listen port. **Never open it to the internet.** |
| `bind_ip` | `127.0.0.1` | The IPv4 address to listen on. Keep `127.0.0.1`: the MasterServer has no authentication. Change it only if the LoginServer runs on another machine; then let the firewall admit port 9010 from that machine only. |

## GameServer (`conf\game_server.conf`)

The game world, plus the community link on a second port.

### Network

| Key | Default | Meaning |
|---|---|---|
| `game_port` | `8002` | Listen port of the game world |
| `system_port` | `game_port` + 1 = `8003` | Listen port of the community link (party, party board, whisper, friends, server-wide megaphones) |
| `bind_ip` | `0.0.0.0` | The IPv4 address both ports listen on |
| `game_ip` | `127.0.0.1` | **The GameServer address sent to the client** on every portal, teleport or town return (the client reconnects then). Also the default of `system_ip`. **Set it to the address players reach.** |
| `system_ip` | same as `game_ip` | The community link address sent to the client. Leave it unset unless the community port is reached at another address. |
| `paula_mode` | `true` | Keep `true` |
| `sbox_path` | `tools/capture/sbox_cora.bin` | The client's packet cipher table |
| `master_ip` | `127.0.0.1` | MasterServer address. The GameServer only exchanges a greeting with it and works without it. |
| `master_port` | `9010` | MasterServer port |

### Gameplay

There are no rate settings: EXP, TM, gelder and drops follow the original tables (1x), plus the
original party bonus.

| Key | Default | Meaning |
|---|---|---|
| `restart_town` | `last` | Where "Go to Town" on the death screen sends a player. `last` = the last town visited (original behavior). `zone` = the town of the map where the player died (`Town` in `db/maps_db.yml`; maps without one fall back to the last town). |
| `post_sync_minutes` | `15` | Minutes between two post box checks for a player (new mail appears then, or when the player opens a list). The original value is 15. `0` or less means 15. |
| `megaphone_delay_ms` | `18000` | Milliseconds between two megaphone broadcasts (the original `MegaphoneDelay`). TBN megaphones and system megaphones wait in the same queue, and one message goes out per interval. `0` = the queue sends nothing: no TBN or system megaphone is broadcast. |
| `megaphone_island_name` | `Paula` | The name shown in the middle of a system megaphone line: `[트릭스터] <name> - <text>` |

### Data files

| Key | Default | Meaning |
|---|---|---|
| `map_path` | `data/map` | Folder of the map files (`.md3`). Files are found by the `FileName` in `db/maps_db.yml`. |

Every game data table has its own path key. You only need them to point the server at modified
data. A file that cannot be loaded is logged as an `ERROR`, and the server starts with that part of
the game missing.

| Key | Default file |
|---|---|
| `boss_mon_db_path` | `db/boss_mon_db.yml` |
| `char_range_path` | `db/char_range_db.yml` |
| `compound_db_path` | `db/compound_db.yml` |
| `compounder_db_path` | `db/compounder_db.yml` |
| `debuff_resist_db_path` | `db/monster_debuff_resist_db.yml` |
| `drill_client_db_path` | `db/drill_client_db.yml` |
| `drill_db_path` | `db/drill_db.yml` |
| `drill_grade_exp_db_path` | `db/drill_grade_exp_db.yml` |
| `equip_property_db_path` | `db/equip_property_db.yml` |
| `esa_db_path` | `db/esa_db.yml` |
| `exchange_shop_db_path` | `db/exchange_shop_db.yml` |
| `exchange_shop_table_db_path` | `db/exchange_shop_table_db.yml` |
| `exp_table_path` | `db/exp_table.fnt2` |
| `gun_db_path` | `db/gun_db.yml` |
| `item_db_path` | `db/item_db.yml` |
| `item_rebirth_db_path` | `db/item_rebirth_db.yml` |
| `item_refine_db_path` | `db/item_refine_db.yml` |
| `item_refine_table_db_path` | `db/item_refine_table_db.yml` |
| `limit_skill_master_db_path` | `db/limit_skill_master_db.yml` |
| `map_attr_db_path` | `db/map_attr_db.yml` |
| `map_item_db_path` | `db/map_item_db.yml` |
| `map_item_event_add_db_path` | `db/map_item_event_add_db.yml` |
| `map_item_table_db_path` | `db/map_item_table_db.yml` |
| `maps_db_path` | `db/maps_db.yml` |
| `maturing_compound_db_path` | `db/maturing_compound_db.yml` |
| `maturing_help_db_path` | `db/maturing_help_db.yml` |
| `memory_port_db_path` | `db/memory_port_db.yml` |
| `mob_db_path` | `db/mob_db.yml` |
| `mon_item_drop_db_path` | `db/mon_item_drop_db.yml` |
| `mon_item_table_db_path` | `db/mon_item_table_db.yml` |
| `mon_spawn_db_path` | `db/mon_spawn_db.yml` |
| `mon_speech_db_path` | `db/mon_speech_db.yml` |
| `monster_quest_db_path` | `db/monster_quest_db.yml` |
| `monster_quest_result_db_path` | `db/monster_quest_result_db.yml` |
| `npc_check_lv_msg_db_path` | `db/npc_check_lv_msg_db.yml` |
| `npc_db_path` | `db/npc_db.yml` |
| `npc_move_db_path` | `db/npc_move_db.yml` |
| `npc_msg_db_path` | `db/npc_msg_db.yml` |
| `party_quest_db_path` | `db/party_quest_db.yml` |
| `party_quest_exp_db_path` | `db/party_quest_exp_db.yml` |
| `party_quest_result_db_path` | `db/party_quest_result_db.yml` |
| `pet_param_db_path` | `db/pet_param_db.yml` |
| `pet_speech_db_path` | `db/pet_speech_db.yml` |
| `post_levelup_gift_db_path` | `db/post_levelup_gift_db.yml` |
| `post_setup_db_path` | `db/post_setup_db.yml` |
| `pre_esaic_condition_db_path` | `db/pre_esaic_condition_db.yml` |
| `present_item_db_path` | `db/present_item_db.yml` |
| `quest_db_path` | `db/quest_db.yml` |
| `quest_result_db_path` | `db/quest_result_db.yml` |
| `random_port_db_path` | `db/random_port_db.yml` |
| `random_result_item_db_path` | `db/random_result_item_db.yml` |
| `random_stat_db_path` | `db/random_stat_db.yml` |
| `random_stat_import_db_path` | `db/random_stat_import_db.yml` |
| `refine_support_db_path` | `db/refine_support_db.yml` |
| `scenario_quest_db_path` | `db/scenario_quest_db.yml` |
| `shop_db_path` | `db/shop_db.yml` |
| `shop_item_db_path` | `db/shop_item_db.yml` |
| `skill_card_db_path` | `db/skill_card_db.yml` |
| `skill_db_path` | `db/skill_db.yml` |
| `skill_master_db_path` | `db/skill_master_db.yml` |
| `skill_scroll_db_path` | `db/skill_scroll_db.yml` |
| `skill_shop_db_path` | `db/skill_shop_db.yml` |
| `skill_shop_item_db_path` | `db/skill_shop_item_db.yml` |
| `stat_table_path` | `db/stat_table.fnt2` |
| `stress_point_db_path` | `db/stress_point_db.yml` |
| `teleport_adv_db_path` | `db/teleport_adv_db.yml` |
| `teleport_db_path` | `db/teleport_db.yml` |
| `tm_level_db_path` | `db/tm_level_db.yml` |
| `trans_scroll_db_path` | `db/trans_scroll_db.yml` |

## Example: a public server

The public address here is `203.0.113.25` and the database user is `paula`. Only the changed keys
are shown.

```
// conf\auth_server.conf
db_port: 3306
db_user: paula
db_pass: a-long-random-password
char_server_ip: 203.0.113.25
island_1: 1, 1, Paula, Ruby Island, 203.0.113.25, 8001

// conf\login_server.conf
db_port: 3306
db_user: paula
db_pass: a-long-random-password
game_server_ip: 203.0.113.25

// conf\master_server.conf
db_port: 3306
db_user: paula
db_pass: a-long-random-password

// conf\game_server.conf
db_port: 3306
db_user: paula
db_pass: a-long-random-password
game_ip: 203.0.113.25
```

Then open TCP 9980, 8001, 8002 and 8003 as described in
[INSTALL.md](INSTALL.md#10-public-hosting).

## Player side: `launcher.ini`

The player's launcher has its own settings file. Its keys are described in
[CONNECTING.md](CONNECTING.md#2-install-paulalauncher).
