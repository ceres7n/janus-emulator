# ProjectPAULA architecture

ProjectPAULA reconstructs the Trickster Online KR 2014 ("Paula") server from the recovered retail
server programs, whose class names and symbols survived. This page describes how the release is
built: the processes, the connection flow, persistence, threading, the game data and the source
tree.

## 1. Principles

- **1:1 with retail.** Packets, formulas, timings and even odd behavior follow the original
  server. Comments that cite `sub_XXXXXX`, `0xADDR`, "retail" or "1:1" record where a rule comes
  from; they are documentation and stay in the code.
- **Real names.** Where a module mirrors a retail class, it keeps the retail name: `cLoginServer`,
  `cMasterServer`, `cGameServer`, `CharStore`, retail table and procedure names.
- **Stored procedures become C++ methods.** The original server kept its game rules for the
  database in SQL stored procedures. ProjectPAULA ports each procedure 1:1 into a method (mostly
  in the store classes and the GameServer modules). The database itself holds only tables.
- **Every S2C packet is verified in the client.** An opcode and its body layout are sent only
  after they were checked in the client binary.
- **Hardening is marked.** A server-side check that the retail server did not have, added because
  a modified client could abuse its absence, carries a comment starting with
  `// Hardening (not retail):`. An unmodified client never notices these checks.

## 2. Processes and ports

```
                         +------------------+
 Player PC               |  AuthServer      |  :9980   account login, island list
 PaulaLauncher.exe       +------------------+
   -> Trickster.bin      +------------------+
        |                |  LoginServer     |  :8001   character list / create / delete / select
        +--------------> +------------------+
                                  |  inter-server link (char data)
                                  v
                         +------------------+
                         |  MasterServer    |  :9010   internal only (127.0.0.1)
                         +------------------+
                         +------------------+
                         |  GameServer      |  :8002   the game world
                         |                  |  :8003   community link (party, whisper,
                         +------------------+          friends, megaphones)
                         +------------------+
                         |  MariaDB         |  shared by all four servers (internal only)
                         +------------------+
```

| Process | Retail counterpart | Role |
|---|---|---|
| `AuthServer.exe` | FirstLoginSvrW | Account lookup (no password in v0.1), refusal of unknown or banned accounts, login token, island list |
| `LoginServer.exe` | LoginServer (`cLoginServer`) | Character list, creation, deletion and selection; game ticket; hands the client to the GameServer |
| `MasterServer.exe` | MasterServer (`cMasterServer`) | Character list / create / delete for the LoginServer, through `CharStore` |
| `GameServer.exe` | GameServer (`cGameServer`) | The world: maps, NPCs, monsters, combat, items, quests. It also runs the services of the retail SystemServer (party, whisper, friends, megaphones) on its community port. |

All four servers use the same MariaDB database. The GameServer reads and writes it directly.

## 3. Connection flow

| Step | Client connects to | How the client learns the address | Main packets |
|---|---|---|---|
| 1 | AuthServer `:9980` | The address baked into the client, replaced in memory by PaulaLauncher | Login: the account name; refused with S2C 11503. Island list S2C 11502. Island choice C2S 11505, answer S2C 11506 with the LoginServer address. |
| 2 | LoginServer `:8001` | S2C 11506 (`island_N` ip and port in `auth_server.conf`) | Token check against `login_tokens`. Character create C2S 2006 (answer S2C 2008, or 2007 with an error code), delete C2S 2009 (answer 2011, or 2010 with an error code), select: S2C 0x07DE with the GameServer address. |
| 3 | GameServer `:8002` | S2C 0x07DE (`game_server_ip` / `game_server_port`) | First enter C2S 0x0004, then the world spawn sequence |
| 4 | GameServer `:8003` | S2C 0x1585 (`system_ip` / `system_port`), a second connection | C2S 0x1581, S2C 0x1582, then party, whisper, friends and megaphone traffic |
| 5 | GameServer `:8002` again | S2C 154 RELOCATE (`game_ip` / `game_port`) on every portal, teleport or town return | C2S 156 re-enter, S2C 437 + 157 |

### Login to Game handoff ticket

Retail handed the selected character to the GameServer over its internal links. ProjectPAULA's
GameServer reads the character itself, so it needs proof that the connecting client may load that
character:

1. On a character select, the LoginServer checks that the character belongs to the session's
   account, then writes a row into `game_tickets`: character uid, account id, the client's IP
   address, and an expiry 300 seconds ahead.
2. On C2S 0x0004 (first enter) and C2S 156 (re-enter), the GameServer accepts the character only
   with an unexpired ticket for that character, from the same IP address, whose account owns the
   character. It deletes the ticket once it is used. Otherwise it logs a warning and closes the
   connection.
3. Before every reconnect the GameServer itself orders (S2C 154), it writes a fresh ticket the
   same way.
4. If the character is still online in another session, that session is saved and closed first.

The community port has its own ticket, sent in S2C 0x1585, bound to an online character from the
same IP address.

## 4. Inter-server link

`src/common/interserver/` holds the server-to-server transport: frames, a byte stream, a dispatcher
and `InterLink` (one TCP link with a receive buffer and a frame loop).

| Opcode | Name | Use |
|---|---|---|
| 3000 / 3001 / 3002 | `SVR_LOGIN`, `SVR_LOGIN_ACK_CANCEL`, `SVR_LOGIN_ACK_OK` | Handshake when a server connects to the MasterServer |
| 40001 / 40002 | `MREQ_CHAR_LIST` / `MACK_CHAR_LIST` | Character list of an account |
| 40003 / 40004 | `MREQ_CHAR_CREATE` / `MACK_CHAR_CREATE` | Create a character |
| 40005 / 40006 | `MREQ_CHAR_DELETE` / `MACK_CHAR_DELETE` | Delete a character |

The 400xx opcodes are ProjectPAULA's own. Requests carry a request id, and the answer is handed
back to the client session that asked. The LoginServer connects to the MasterServer at start;
while the link is down (not up yet, or lost) it dials again every 3 seconds, and a request made
meanwhile fails at once instead of waiting. The GameServer's link to the MasterServer is a
handshake only: one attempt at start, nothing depends on it.

Character loading in the GameServer deliberately does not go through the MasterServer: all servers
share one database, so a relay would add latency and complexity on the login path without any
benefit.

## 5. Persistence

- **One MariaDB database** (default name `janus`, a historical name) created by `sql/001_*.sql` to
  `sql/019_*.sql` (`install-db.bat` runs them in order; every script can run again safely).
- **Store classes** in `src/common/store/` hold the ported procedures:

  | Class | Tables | Retail procedures (examples) |
  |---|---|---|
  | `CharStore` | `characters` and the per-character rows | `usp_insert_character` (creation with the starter kit), `uspm_char_all_select_char_slot_info` (list), `uspm_deleted_char_insert` (deletion), name change, state saving |
  | `ItemStore` | `char_inventory`, `char_item_option` | Inventory, warehouse and bank moves, trade |
  | `PostStore` | `post`, `post_date`, `post_item` | Post box, gifts, level gifts |

- Other GameServer modules run their SQL directly (skills in `char_skills`, quests, friends,
  memory port, megaphone queue and so on), again following the retail procedures.
- **Saving:** the GameServer saves every in-world player every 15 seconds, on every map change,
  at logout and when it stops. Item changes are written when they happen.

## 6. Threading and timers

- Networking uses standalone **asio**. Each session handles its packets on its own strand, so one
  session's handlers never run at the same time.
- Auth, Login and Master use `io_threads` worker threads (default 2).
- **The GameServer runs on exactly one network thread in v0.1.** Every session strand, the world
  timers and the shared managers are therefore serialized, like the retail server's single world
  thread.
- GameServer timers include the 15-second autosave, the monster AI and regeneration ticks, item
  and drop timers, effect-state (buff) timers and the megaphone queue.
- A handler that throws is logged and the server keeps running.
- Stop: Ctrl+C, Ctrl+Break, closing the console window and a system shutdown all run the same
  stop path. The GameServer saves every online player on its own thread before it exits.

## 7. Packets

- The client and the servers talk through a framed protocol with an 11-byte header. Frames are
  ciphered with a 65,536-byte substitution table, `tools/capture/sbox_cora.bin` (the client's
  `g_crypt_table`). `paula_mode: true` selects this protocol.
- `src/common/opcodes.hpp` names the known opcodes. Opcodes without a name are dispatched by number
  in `game_session.cpp`.
- `src/common/packet.*` is the packet buffer; `src/common/session.hpp` is the TCP session (receive,
  frame, decipher, dispatch, send queue).
- Text sent to the client is converted from UTF-8 to the client's code page, CP949
  (`src/common/client_text.*`).

## 8. Game data

| Path | Contents | Read by |
|---|---|---|
| `db/*.yml` (67 files) | Game tables: items, monsters, maps, NPCs, dialogs, shops, skills, quests, drops, drill, compounds, pets and more. Mostly converted from the original server data (`kto_libconfig.xml`), some from the client data. | `src/gameserver/world/*_db.hpp` |
| `db/stat_table.fnt2`, `db/exp_table.fnt2` | The retail stat and EXP tables | `src/gameserver/world/stat_table.hpp` |
| `data/map/*.md3` (685 files) | Map attribute grids: blocked cells, ground types, points | `src/gameserver/world/map_attr.hpp` |
| `tools/capture/sbox_cora.bin` | The packet cipher table | `src/common/paula_crypt.*` |

The YAML files use one layout:

```
# header comment: source table, field meanings, retail use
Header:
  Type: GUN_DB
  Version: 1

Body:
  - ID: ...
    FieldName: value

Footer:
  Imports:
```

- Field names are CamelCase and keep the original column names.
- Ids are numeric, as in the original data.
- Every data file has its own path key in `conf/game_server.conf`
  ([CONFIGURATION.md](CONFIGURATION.md#data-files)).
- A file that fails to load is logged as an `ERROR`; the server starts without that part.

## 9. Source layout

```
ProjectPAULA/
  CMakeLists.txt          root build: C++17, Win32 only, exes into the project root
  3rdparty/               asio 1.30.2 (headers) and MySQL Connector/C 6.1.11 (x86)
  conf/                   one .conf per server
  data/map/               the .md3 map files
  db/                     the game data tables (YAML, fnt2)
  doc/                    documentation
  sql/                    database scripts 001..019 (+ README.md)
  tools/
    capture/sbox_cora.bin the packet cipher table
    launcher/             PaulaLauncher (player side)
    add-account.bat       creates an account
  install-db.bat          creates or updates the database
  run-server.bat          starts the four servers
  stop-server.bat         stops them (the GameServer saves first)
  src/
    common/               shared by all servers
      config.*            conf file reader
      logger.*            console + <prefix>_live.log, log_level
      database.*          MariaDB access (MySQL C API)
      session.*           TCP session: frames, cipher, send queue
      packet.*, opcodes.hpp, paula_crypt.*, client_text.*
      server_runtime.*    start-up and stop plumbing shared by the four mains
      game_constants.hpp, version.hpp
      interserver/        server-to-server link
      store/              CharStore, ItemStore, PostStore
    authserver/           AuthServer (account lookup, islands)
    loginserver/          LoginServer (character select, game ticket)
    masterserver/         MasterServer (character data service)
    gameserver/           GameServer
      main.cpp            start, crash dumps, stop
      game_server.*       the server: ports, timers, world managers, autosave
      game_session.*      one client: packet dispatch, enter/relocate, movement, saving
      system_session.*    the community port (8003)
      world/              data loaders (*_db.hpp) and the entity manager
      combat_util.hpp, progress.cpp, gun.cpp          combat, EXP/TM, guns
      skill.cpp, skill_<line>.cpp, es.cpp             skills per class line, effect states (buffs)
      regen.cpp, monster_ai.cpp, drop.cpp             monster spawns, AI, drops
      inventory.cpp, equip.cpp, item_use.cpp          bag, equipment, item use
      shop.cpp, exchange_shop.cpp, trade.cpp          NPC shops, exchange shops, trade
      warehouse.cpp, bank.cpp, compound.cpp, refine.cpp
      drill.cpp, memory_port.cpp, pet.cpp
      npc_talk.cpp, quest.cpp, monster_quest.cpp, party_quest.cpp, skill_shop.cpp
      party.cpp, chat_server.cpp, friend_server.cpp, megaphone.cpp, postbox.cpp
      atcommand.cpp, skill_gm.cpp                      GM commands and GM skills
```

## 10. Build and runtime files

- CMake builds the static library `paula_common`, the four servers (targets `authserver`,
  `loginserver`, `masterserver`, `gameserver`) and `PaulaLauncher`. The exes and `libmysql.dll` are
  written to the project root, next to `conf/`, `db/`, `data/` and `tools/`.
- Each server makes the folder of its exe its working folder at start, so the relative paths
  work however it is started.
- Runtime files in the project root: `<prefix>_live.log` (`master`, `auth`, `login`, `game`;
  each server overwrites its own at start, and `run-server.bat` first moves the previous ones to
  `logs\`) and, after a GameServer crash, `GameServer_crash_<date>_<time>.dmp` (read it with the
  matching `GameServer.pdb`).
- Each server logs `ProjectPAULA <ServerName> v0.1.0` at start (`src/common/version.hpp`).
