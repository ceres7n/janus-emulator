# Installing a ProjectPAULA server on Windows

This guide sets up a complete server on one Windows PC: the four server programs plus MariaDB.
It covers a private test server and a public server that players reach over the internet.

Contents:
1. [What you will set up](#1-what-you-will-set-up)
2. [Requirements](#2-requirements)
3. [Get the server programs](#3-get-the-server-programs)
4. [Install MariaDB](#4-install-mariadb)
5. [Create the database](#5-create-the-database)
6. [Give the servers their own database user](#6-give-the-servers-their-own-database-user)
7. [Configure the servers](#7-configure-the-servers)
8. [Accounts, bans and game masters](#8-accounts-bans-and-game-masters)
9. [Start and stop](#9-start-and-stop)
10. [Public hosting](#10-public-hosting)
11. [Logs and crash dumps](#11-logs-and-crash-dumps)
12. [Backups and updates](#12-backups-and-updates)
13. [Troubleshooting](#13-troubleshooting)

## 1. What you will set up

| Program | Port | Job | Reachable by players? |
|---|---|---|---|
| `AuthServer.exe` | TCP 9980 | Account login and the island list | Yes |
| `LoginServer.exe` | TCP 8001 | Character list, creation, deletion and selection | Yes |
| `GameServer.exe` | TCP 8002 | The game world | Yes |
| `GameServer.exe` | TCP 8003 | Community link: party, whisper, friends, megaphones | Yes |
| `MasterServer.exe` | TCP 9010 | Character data service for the LoginServer | **No, never** |
| MariaDB | TCP 3306 or 3307 | Database | **No, never** |

All four servers share one MariaDB database. The client connects to the AuthServer first, then the
servers tell it where to go next (LoginServer, then GameServer). That is why the address players
reach must be set in the server configuration (section 7.2).

## 2. Requirements

| What | Notes |
|---|---|
| Windows 10 or 11 | The servers are 32-bit programs. They run on 64-bit Windows. |
| The latest Microsoft Visual C++ v14 Redistributable **(x86)** | The package for Visual Studio 2017-2026: `vc_redist.x86.exe` from <https://aka.ms/vc14/vc_redist.x86.exe>. Install the x86 package even on 64-bit Windows. The servers are built with Visual Studio 2026 and need a package at least that new: an older "2015-2022" package can be too old. Without it the servers do not start. |
| MariaDB 10.2 or newer | Tested with MariaDB 11.4 and 11.8. **MySQL 8.4 and 9.x are not supported**: the bundled client library (MySQL Connector/C 6.1.11) cannot log in to them. See `sql\README.md`. |
| About 120 MB of disk | Most of it is game data (`db\` and `data\map\`, about 84 MB) and `GameServer.pdb` (about 30 MB). A build from source needs about 250 MB more. |
| To build from source | Visual Studio 2026 with the **"Desktop development with C++"** workload. It includes the 32-bit compiler and CMake. Visual Studio 2022 should also work but is not tested. You also need Git to clone the repository. |

## 3. Get the server programs

The source code is at <https://github.com/ceres7n/janus-emulator> (the repository is named
`janus-emulator`; the project is ProjectPAULA). Build it yourself (section 3.2), or use the
optional prebuilt package from the
[release page](https://github.com/ceres7n/janus-emulator/releases) (section 3.1).

### 3.1 Prebuilt package (optional)

The release page offers two packages:

| Package | Contents | For |
|---|---|---|
| `ProjectPAULA-v0.1.0-server-win32.zip` | The four server programs, `libmysql.dll`, `conf\`, `db\`, `data\`, `sql\`, `tools\`, the `.bat` scripts and the documentation | The server operator |
| `ProjectPAULA-v0.1.0-launcher.zip` | `PaulaLauncher.exe`, `launcher.ini` and a short README | The players ([CONNECTING.md](CONNECTING.md)) |

1. Download `ProjectPAULA-v0.1.0-server-win32.zip`.
2. Extract it to a short path, for example `C:\`. The zip holds one folder, so the server ends up in
   `C:\ProjectPAULA-v0.1.0-server-win32` (you can rename that folder).
3. Continue with [section 4](#4-install-mariadb).

### 3.2 Build from source

1. Clone the repository into a **short path**, for example `C:\ProjectPAULA`. In a command
   prompt or PowerShell:
   ```
   git clone https://github.com/ceres7n/janus-emulator.git C:\ProjectPAULA
   ```
   Very deep paths (around 200 characters inside the build folder) break the Visual Studio build
   tools with errors such as `MSB6003` or "No CMAKE_CXX_COMPILER could be found".
2. Open **"Developer PowerShell for VS 2026"** (Start menu) and go to the project folder:
   ```
   cd C:\ProjectPAULA
   ```
3. Configure. `-A Win32` is required: the bundled MySQL library is 32-bit, and a 64-bit configure
   stops with an error.
   ```
   cmake -S . -B build -A Win32
   ```
4. Build the Release configuration (about 3 minutes):
   ```
   cmake --build build --config Release
   ```
5. The programs land in the project folder, next to `conf\`, `db\` and `data\`:
   `AuthServer.exe`, `LoginServer.exe`, `MasterServer.exe`, `GameServer.exe`, `GameServer.pdb`,
   `PaulaLauncher.exe` and `libmysql.dll` (copied from `3rdparty\`).

Notes:
- Build **Release**. A Debug build writes the same file names and overwrites the Release ones.
- Everything the build needs is in the repository (`3rdparty\` holds asio and MySQL Connector/C).
  Nothing else has to be downloaded.
- Keep `GameServer.pdb` together with the `GameServer.exe` built with it. It is needed to read
  crash dumps (section 11).

## 4. Install MariaDB

1. Install MariaDB from the official MSI package. During setup, **set a root password** and note
   the port:
   - A standalone MariaDB uses **3306**.
   - WampServer's MariaDB uses **3307**, which is the default in ProjectPAULA's `conf\*.conf`.
2. Make MariaDB accept connections from this PC only. In its `my.ini` (for the MSI package it is
   in the MariaDB `data` folder), under `[mysqld]`, add:
   ```
   bind-address=127.0.0.1
   ```
   Then restart the MariaDB service.
3. Do **not** open or forward the database port.

## 5. Create the database

1. Open `install-db.bat` (in the project folder) in a text editor and check the settings at the
   top:

   | Variable | Meaning | Default |
   |---|---|---|
   | `MYSQL` | Full path of `mysql.exe` or `mariadb.exe`, or just `mysql.exe` if it is on PATH | `mysql.exe` |
   | `DB_HOST` | Database host | `127.0.0.1` |
   | `DB_PORT` | Database port | `3307` |
   | `DB_USER` | A user that may create databases, tables and (temporary helper) procedures, for example `root` | `root` |
   | `DB_PASS` | That user's password. Avoid the characters `%` `!` `^` `&` `"` in it. | empty |
   | `DB_NAME` | Name of the database to create: letters, digits and `_` only | `janus` |

   The MSI package installs the client in the MariaDB `bin` folder (for example
   `C:\Program Files\MariaDB 11.4\bin\mysql.exe`). Instead of editing the file, you can also set
   the environment variables `PAULA_MYSQL`, `PAULA_DB_HOST`, `PAULA_DB_PORT`, `PAULA_DB_USER`,
   `PAULA_DB_PASS` and `PAULA_DB_NAME`; they override the values in the file.
2. Save the file and run it (double-click, or run it from a command prompt).
   It first checks the connection, creates the database if it does not exist (utf8mb4), then runs
   every `sql\NNN_*.sql` file in number order (`001_...` to `019_...`) and stops at the first
   error. At the end it shows how many scripts it applied.
3. Running it again is safe. Every script can run more than once: tables are created only if
   missing, columns are added only if missing, and seed rows are inserted only once. This is also
   how you update the database to a newer release (section 12).

Notes:
- The database name `janus` is historical. You can choose another name, as long as you use the
  same name in `install-db.bat` and in `db_name` in all four `conf\*.conf` files.
- No account is created by the scripts. You create accounts yourself (section 8).
- `sql\README.md` describes every script and how to upgrade an older database by hand.
- The MariaDB 11 client may print `WARNING: option --ssl-verify-server-cert is disabled, because
  of an insecure passwordless login.` when the user has no password. It is harmless.

## 6. Give the servers their own database user

The servers only read and write rows. They need `SELECT`, `INSERT`, `UPDATE` and `DELETE`, nothing
more. Do not run them as `root`.

1. Open the MariaDB client as root (use your port):
   ```
   mysql -h 127.0.0.1 -P 3306 -u root -p
   ```
2. Create the user (choose your own password; replace `janus` if you chose another database name):
   ```sql
   CREATE USER 'paula'@'127.0.0.1' IDENTIFIED BY 'a-long-random-password';
   GRANT SELECT, INSERT, UPDATE, DELETE ON janus.* TO 'paula'@'127.0.0.1';
   ```
3. Put this user into **all four** conf files (section 7.1).

Keep using `root` (or another admin user) in `install-db.bat`: it creates tables and needs more
rights.

The password must not contain `#` or `//`: the conf reader treats them as the start of a comment
and cuts the value there (see [CONFIGURATION.md](CONFIGURATION.md#file-syntax)).

## 7. Configure the servers

The settings are in four text files in `conf\`. Every key is described in
[CONFIGURATION.md](CONFIGURATION.md). For a normal install you change only the keys below.

### 7.1 Database settings (all four files)

Set the same values in `conf\auth_server.conf`, `conf\login_server.conf`,
`conf\master_server.conf` and `conf\game_server.conf`:

| Key | Default | Set to |
|---|---|---|
| `db_host` | `127.0.0.1` | Keep `127.0.0.1` |
| `db_port` | `3307` | Your MariaDB port (`3306` for a standalone MariaDB) |
| `db_user` | `root` | The server user from section 6, for example `paula` |
| `db_pass` | empty | That user's password |
| `db_name` | `janus` | Your database name |

### 7.2 The address players reach

For a test on one PC, leave every address at `127.0.0.1`.

When players connect from other PCs, the servers must tell the client the address it can reach:
your **public IPv4 address** for internet players, or the server's LAN address for a LAN-only
server. It goes in four places:

| File | Key | What the client does with it |
|---|---|---|
| `conf\auth_server.conf` | `island_1`, **5th field** | Connects to the LoginServer at this address after the island select |
| `conf\auth_server.conf` | `char_server_ip` | Same, but only used when there is no `island_N` line. Set it anyway. |
| `conf\login_server.conf` | `game_server_ip` | Connects to the GameServer at this address after the character select |
| `conf\game_server.conf` | `game_ip` | Reconnects here after every portal or teleport, and opens the community link (port 8003) here |

Example for the public address `203.0.113.25`:

```
// conf\auth_server.conf
char_server_ip: 203.0.113.25
island_1: 1, 1, Paula, Ruby Island, 203.0.113.25, 8001

// conf\login_server.conf
game_server_ip: 203.0.113.25

// conf\game_server.conf
game_ip: 203.0.113.25
```

Rules:
- Use a numeric IPv4 address (digits and dots, at most 15 characters). Host names do not work.
- `system_ip` in `conf\game_server.conf` follows `game_ip`. Leave it commented out.
- Keep `master_ip` and `db_host` at `127.0.0.1`.
- Players enter the same address as `LoginIP` in their `launcher.ini`.

### 7.3 Keys to leave alone

| Key | Why |
|---|---|
| `port: 9980` in `auth_server.conf` | The client always connects to port 9980 |
| `paula_mode: true` | The KR 2014 client speaks only this protocol. `false` is not supported (the AuthServer refuses to start with it). |
| `sbox_path` | The client's packet cipher table |
| `io_threads` in `game_server.conf` | The v0.1 GameServer always runs on 1 network thread |
| `bind_ip` in `master_server.conf` | Keeps the MasterServer reachable from this PC only |

## 8. Accounts, bans and game masters

### 8.1 Create accounts

There is no registration page. The operator creates every account:

```
tools\add-account.bat <account name> [gm_level]
```

- `tools\add-account.bat` has **its own copy** of the database settings at its top (`MYSQL`,
  `DB_HOST`, `DB_PORT`, `DB_USER`, `DB_PASS`, `DB_NAME`, with the same defaults as
  `install-db.bat`). Set the same values there once, or set the `PAULA_DB_*` environment
  variables of section 5.
- The name has 1 to 18 characters: letters `A-Z` `a-z`, digits and `_`. Upper and lower case
  count as the same name.
- `gm_level` is optional: a number from 0 to 255, default 0. See section 8.3 for what it does.
- On success the script prints the command line the player uses (`PaulaLauncher.exe <name>`) and
  the SQL that makes a character a GM. `ERROR 1062 "Duplicate entry"` means the name is taken;
  `ERROR 1146 "doesn't exist"` means the tables are missing (run `install-db.bat`).

**There are no passwords in v0.1.0.** The account name is the only key: whoever knows it can play
that account. So:
- Give each player their own account.
- Use names nobody can guess, for example 12 or more random letters and digits, and send them to
  the players privately, like passwords.
- An unknown account name is refused: the client shows a "LOGIN ERROR" message box.

The same with SQL, if you prefer:

```sql
INSERT INTO accounts (username, password_hash) VALUES ('k7Qm2xVb9Lp4', '');
```

`password_hash` must be given because the column has no default, but v0.1.0 does not check it.

Each account has 3 character slots (`accounts.usable_slot`, default 3).

### 8.2 Ban an account

A banned account is refused at login with the same "LOGIN ERROR" box.

```sql
-- Permanent ban
UPDATE accounts SET banned = 1, ban_until = NULL, ban_reason = 'botting' WHERE username = 'name';
-- Ban until a date
UPDATE accounts SET banned = 1, ban_until = '2026-12-01 00:00:00' WHERE username = 'name';
-- Lift the ban
UPDATE accounts SET banned = 0 WHERE username = 'name';
```

The ban is checked when the account logs in. It does not disconnect a player who is already in
the game.

### 8.3 Game masters

Two different settings exist. Only the second one gives GM commands.

| Setting | Effect |
|---|---|
| `accounts.gm_level` > 0 | The account also sees islands marked "GM only" in the island list. Nothing else. |
| `characters.authority` = 1 | That **character** can use all `@` GM commands and wear GM-only items. |

To make a character a GM, create it in the game first, then:

```sql
UPDATE characters SET authority = 1 WHERE name = 'CharacterName' AND deleted = 0;
```

The player must log out and in again. **Never set `authority` to 2**: 2 marks a deleted character,
and it disappears from the character list. The commands are in [GM_COMMANDS.md](GM_COMMANDS.md).

## 9. Start and stop

### 9.1 With the scripts

| Script | What it does |
|---|---|
| `run-server.bat` | Starts MasterServer, then AuthServer, LoginServer and GameServer, each in its own window |
| `stop-server.bat` | Stops the servers. The GameServer saves every online player before it exits. |

`run-server.bat` in detail:
1. It checks that the four `.exe` files, `libmysql.dll`, the four `conf\*.conf` files,
   `tools\capture\sbox_cora.bin`, `db\` and `data\map\` are there.
2. It refuses to start while a server started from this folder is still running: it never starts
   a second copy and never stops a running one. Stop the servers with `stop-server.bat` first.
3. It moves the logs of the previous run into `logs\` (section 11).
4. It starts the MasterServer and waits up to 30 seconds until it listens on port 9010, then
   starts the AuthServer, the LoginServer and the GameServer, and waits up to 180 seconds until
   the GameServer listens on port 8002 (it loads its data first).
5. If a server does not come up, the script prints the last lines of that server's log and
   waits for a key.

The ports it waits for are set at its top (`MASTER_PORT`, `GAME_PORT`). If you change
`master_port` or `game_port` in the conf files, change them there too.

`stop-server.bat` stops only the servers started from this folder, in the order GameServer,
LoginServer, AuthServer, MasterServer. It asks each one to close, the same way as closing its
window, and waits up to 15 seconds. A server that is still running then is forced to stop, with
a warning: a forced GameServer loses what changed since its last autosave.

When a server starts, its window shows `ProjectPAULA <ServerName> v0.1.0`, then the address and
port it listens on. The GameServer loads its data first (maps, NPCs, monsters, shops), so it is
ready a little later than the others.

### 9.2 By hand

Start the programs from the project folder in this order:

1. `MasterServer.exe`
2. `AuthServer.exe`
3. `LoginServer.exe`
4. `GameServer.exe`

The LoginServer needs the MasterServer for the character list. While the MasterServer is not
reachable, the LoginServer tries again every 3 seconds, and after a MasterServer restart it
reconnects by itself. Start the MasterServer first anyway: until the link is up, players who
reach the character select are disconnected.

Each program uses the folder of its `.exe` as its working folder, so it finds `conf\`, `db\` and
`data\` wherever you start it from. Each one also accepts the path of another conf file as its
first argument, for example `GameServer.exe conf\test_game.conf` (a relative path there is
relative to the folder you start it from).

Only one copy of each server can run. A second copy cannot open its port and exits with an error.

### 9.3 Stopping safely

- **Ctrl+C** in a server window, closing the window, or `stop-server.bat` all stop a server
  normally. The GameServer then saves every online player before it exits.
- Ctrl+C in the GameServer window is the safest way: it has no time limit. When a console window
  is closed (and `stop-server.bat` closes it the same way), Windows ends the program about
  5 seconds later, finished or not.
- `taskkill /F` (or ending the process in Task Manager) skips that last save.
- The GameServer also saves every player every **15 seconds**, on every map change and at logout.
  Item changes are written immediately. A crash or a forced kill loses at most about 15 seconds
  of progress.

## 10. Public hosting

1. **Find your public IPv4 address**, for example on your router's status page.
   Compare it with the router's WAN address. If they differ, your provider uses carrier-grade NAT
   and port forwarding cannot work: use a VPS or a tunnel service instead.
2. **Set the address** in the four places of section 7.2, then restart the servers.
3. **Forward TCP ports 9980, 8001, 8002 and 8003** on the router to the server PC's LAN address.
   No UDP is used.
4. **Allow them in the Windows Firewall** and block the internal ports. In a command prompt run as
   administrator:
   ```
   netsh advfirewall firewall add rule name="ProjectPAULA game ports" dir=in action=allow protocol=TCP localport=9980,8001-8003
   netsh advfirewall firewall add rule name="ProjectPAULA block MasterServer" dir=in action=block protocol=TCP localport=9010
   netsh advfirewall firewall add rule name="ProjectPAULA block MariaDB" dir=in action=block protocol=TCP localport=3306,3307
   ```
   The block rules do not affect the servers themselves: they reach the MasterServer and MariaDB
   through `127.0.0.1`.
5. **Never forward 9010 or the database port.** The MasterServer has no authentication; that is
   why its `bind_ip` defaults to `127.0.0.1`.
6. **Test from outside**, for example from a friend's PC or a phone hotspot.
7. **Tell the players** your public IPv4 address (their `LoginIP`) and their account names, and
   point them to [CONNECTING.md](CONNECTING.md).

Good to know:
- The servers send the same public address to every client. A player on the server's own LAN (or
  on the server PC itself) can use the public address only if the router supports NAT loopback
  ("hairpinning").
- When a player selects a character, the LoginServer gives that player a one-time ticket for the
  GameServer, bound to the player's IP address and valid for 300 seconds. A player whose public IP
  address changes between connections (some mobile networks do this) is refused when entering the
  world. The GameServer log then shows a warning.

## 11. Logs and crash dumps

| File | Written by | Notes |
|---|---|---|
| `master_live.log`, `auth_live.log`, `login_live.log`, `game_live.log` | Each server, in the project folder | The log of the current run. A server **overwrites** its log when it starts. |
| `logs\YYYYMMDD_HHMMSS_<name>_live.log` | `run-server.bat` | Before it starts the servers, `run-server.bat` moves the logs of the previous run here, with the date and time of the move. Delete old ones from time to time. |
| `GameServer_crash_YYYYMMDD_HHMMSS.dmp` | The GameServer when it crashes | A minidump. Reading it needs the `GameServer.pdb` built together with that `GameServer.exe`. The log also gets a line with the crash address. |

A server started by hand (section 9.2) overwrites its old log: copy the log first if you need it,
for example for a bug report.

The detail level is set with `log_level` in each conf file:

| Level | What is logged |
|---|---|
| `info` (default) | Start and stop, settings, connections, logins and logouts, characters entering and leaving, map changes, GM command use, refused requests, and an audit line for every trade, mail, bank and shop transaction |
| `debug` | Everything above, plus every packet, move, hit and timer. Very large and slower: use it only while chasing a bug. |
| `warn` / `error` | Only problems |

## 12. Backups and updates

### 12.1 Back up

Stop the servers (or accept a few seconds of inconsistency), then dump the database:

```
mysqldump -h 127.0.0.1 -P 3306 -u root -p --single-transaction janus > janus_backup.sql
```

(`mariadb-dump` is the same tool under its new name.) Also keep a copy of your `conf\` folder.

To restore, create an empty database and load the file:

```
mysql -h 127.0.0.1 -P 3306 -u root -p janus < janus_backup.sql
```

### 12.2 Update to a newer release

1. Run `stop-server.bat`.
2. Back up the database and `conf\` (section 12.1).
3. Get the new programs:
   - prebuilt package: replace the programs and the `db\`, `data\`, `sql\` and `tools\` folders
     with the new ones;
   - from source: run `git pull` in the project folder, then build again (section 3.2). A `build\`
     folder configured before v0.1.0 must be deleted first (section 13).
4. Compare your `conf\*.conf` with the new ones and copy over any new keys. If `git pull` refuses
   to overwrite conf files you changed, copy them somewhere safe, run `git checkout -- conf`, pull
   again, and put your values back.
5. Run `install-db.bat` again. It applies the new `sql\` scripts; the old ones change nothing.
6. Run `run-server.bat`.

A database made before v0.1.0 is upgraded the same way: `install-db.bat` adds what is missing,
for example the `game_tickets` table, and widens `accounts.username` to 18 characters.
A database made with janus-emulator may still hold its default `admin` account: with no
passwords, anyone who types `admin` gets it. Rename or delete it as `sql\README.md` describes.

## 13. Troubleshooting

Look in the server's `*_live.log` first: almost every problem leaves an `ERROR` or `FATAL` line.

| Symptom | Likely cause and fix |
|---|---|
| A server window opens and closes at once | Read its `*_live.log` (`run-server.bat` prints its last lines). Common causes: the database is not running or the `db_*` settings are wrong; the port is already in use (another copy is running, or another program uses the port); the Visual C++ Redistributable (x86) is missing or older than the build (then Windows shows a "DLL not found" or "entry point not found" message instead): install the latest v14 package (section 2). |
| `run-server.bat` says a server is already running | A server started from this folder is still running. Stop the servers with `stop-server.bat`, then start them again. |
| `Access denied for user` in a log | Wrong `db_user` / `db_pass`, or the user was created for another host. Create it for `'127.0.0.1'` (section 6); if it still fails, also create the same user for `'localhost'`. |
| The servers cannot log in to the database at all | MySQL 8.4 and 9.x are not supported: the bundled MySQL Connector/C 6.1.11 works with MariaDB 10.2 or newer. Use MariaDB (see `sql\README.md`). |
| CMake stops with `Missing ...: PAULA_3RDPARTY (...) must hold asio/ and mysql/`, or a build after an update still uses old library files | The `build\` folder was configured before v0.1.0 and keeps the old cached `PAULA_3RDPARTY` path. Delete `build\` (or run `cmake --fresh -S . -B build -A Win32`), then configure and build again. |
| The build fails with `MSB6003`, "No CMAKE_CXX_COMPILER could be found" or errors about long paths | The folder path is too long for the Visual Studio build tools (MSBuild). Clone the repository to a short path such as `C:\ProjectPAULA` and build there. |
| CMake stops with `ProjectPAULA must build 32-bit` | Configure with `-A Win32` (section 3.2). If `build\` was configured without it before, delete `build\` first. |
| The client shows a "LOGIN ERROR" box | The account does not exist or is banned. Check the spelling of the account name in the player's `launcher.ini` against the `accounts` table. |
| The client cannot connect at all | Wrong `LoginIP` in the player's `launcher.ini`, port 9980 not forwarded or blocked, or the AuthServer is not running. |
| The island list appears, then choosing "Ruby Island" fails | The 5th field of `island_1` is wrong, or port 8001 is blocked. |
| The character list does not appear (the client is disconnected after the island select) | The LoginServer has no link to the MasterServer: `login_live.log` shows `connect to Master ... failed` or `no Master link`. Start the MasterServer (keep `master_ip` / `master_port` as they are); the LoginServer reconnects by itself within 3 seconds. |
| Selecting a character fails | `game_server_ip` is wrong or port 8002 is blocked. If `game_live.log` shows a refused game ticket, the player's IP address changed or more than 300 seconds passed: log in again. |
| Players drop at their first portal or teleport | `game_ip` is still `127.0.0.1`. |
| "Party system is not available due to disconnection from the Server" | Port 8003 is blocked, or `game_ip` / `system_ip` is wrong. Party, whisper, friends and the server-wide megaphones need this link. |
| A setting seems to be ignored, or a password is wrong | The conf reader cuts a value at `#` or `//`. Do not use those in values. |
| The world is empty (no NPCs or monsters) | `db\` or `data\map\` is missing or incomplete. The GameServer logs an `ERROR` line for each data file it cannot load. |
| The island window shows two buttons without names | `auth_server.conf` has exactly two `island_N` lines. The client then uses its old two-world window. Use one line, or three or more. |
