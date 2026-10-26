# ProjectPAULA

ProjectPAULA is a server emulator for **Trickster Online KR 2014**, the "Paula" server generation.
It is rebuilt from the recovered retail server programs, and it aims to behave **1:1 like the
original**, quirks included. It is written in C++17, runs on Windows as 32-bit programs, and
stores its data in MariaDB.

**Status: v0.1.0, the first public beta (2026-10-26).** Most of the game works and has been
played. Some systems are missing, and a few known limits matter for anyone who hosts a server.
Read [doc/FEATURES.md](doc/FEATURES.md) before you open a server to other people.

Source code: <https://github.com/ceres7n/janus-emulator>. Prebuilt packages are on its
[release page](https://github.com/ceres7n/janus-emulator/releases).

## Disclaimer

- ProjectPAULA is a **fan-made, non-commercial preservation project**.
- It is **not affiliated with, endorsed by or sponsored by** the owners or publishers of
  Trickster Online. All trademarks belong to their owners.
- **No game client is distributed** here. Each player needs their own client folder
  (see [doc/CONNECTING.md](doc/CONNECTING.md)).
- The folders `db/` and `data/` (and the file `tools/capture/sbox_cora.bin`) hold game data that
  comes from the original game. The server needs these files to run. The details are in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## What works

A short summary. The full status list is in [doc/FEATURES.md](doc/FEATURES.md).

| Area | In v0.1.0 |
|---|---|
| Account and characters | Login, island select ("Ruby Island"), character list, creation and deletion |
| World | Maps with seamless portals and teleports, NPC dialogs and scripts, monster spawns and monster AI |
| Combat | Melee, guns, class skills of all 9 races up to the 3rd job, buffs and debuffs, party skills |
| Progress | EXP and TM per hit or kill, level up to 400, TM level up to 500, death, "Go to Town" and revive |
| Items | Drops, bag, 23 equipment slots, rare items, potions, pets, drill, NPC shops, warehouse, account bank, compound, refine |
| Social | Chat, whisper, friends, party, party quests, post box, trade between players, megaphones |
| Quests | Normal, scenario and monster quests |
| Administration | 18 GM commands ([doc/GM_COMMANDS.md](doc/GM_COMMANDS.md)) |

Not in v0.1.0: guilds, couples, MyCamp, PvP, chat rooms, card and throwing items, rankings and
account passwords. The full list is in [doc/FEATURES.md](doc/FEATURES.md).

## How it fits together

| Program | Port | Job | Open to players? |
|---|---|---|---|
| `AuthServer.exe` | TCP 9980 | Account login and the island list | Yes |
| `LoginServer.exe` | TCP 8001 | Character list, creation, deletion and selection | Yes |
| `GameServer.exe` | TCP 8002 | The game world | Yes |
| `GameServer.exe` | TCP 8003 | Community link: party, whisper, friends, megaphones | Yes |
| `MasterServer.exe` | TCP 9010 | Character data service for the LoginServer | **No, internal** |
| MariaDB | TCP 3307 (default) | Database | **No, internal** |
| `PaulaLauncher.exe` | - | Player side: starts the client and points it at the server | - |

More detail: [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md).

## Requirements

**Server PC**
- Windows 10 or 11. The servers are 32-bit programs and run on 64-bit Windows.
- The latest **Microsoft Visual C++ v14 Redistributable (x86)** (for Visual Studio 2017-2026,
  `vc_redist.x86.exe` from <https://aka.ms/vc14/vc_redist.x86.exe>). You need the x86 package
  even on 64-bit Windows. The servers are built with Visual Studio 2026, so an older
  "2015-2022" package can be too old.
- **MariaDB 10.2 or newer.** Tested with 11.4 and 11.8. MySQL 8.4 and 9.x are not supported:
  the bundled MySQL client library (Connector/C 6.1.11) cannot log in to them
  ([sql/README.md](sql/README.md)).

**To build from source** (optional if you use a prebuilt package)
- Visual Studio 2026 with the "Desktop development with C++" workload. CMake 3.20 or newer
  is included with it.
- Git, to clone the repository.
- A short folder path, for example `C:\ProjectPAULA`. Very deep paths break the build.

**Players**
- Windows and their own Trickster Online KR 2014 client folder. The reference client build is
  described in [doc/CONNECTING.md](doc/CONNECTING.md).
- `PaulaLauncher.exe` and `launcher.ini` from this project, for example from
  `ProjectPAULA-v0.1.0-launcher.zip` on the release page.

## Quick start (everything on one PC)

This gets a private test server running on `127.0.0.1`. To let other people connect, follow
[doc/INSTALL.md](doc/INSTALL.md) instead.

1. Install MariaDB and the Visual C++ Redistributable (x86).
2. Get the programs, in one of two ways:
   - **From source:** open a "Developer PowerShell for VS 2026" and run:
     ```
     git clone https://github.com/ceres7n/janus-emulator.git C:\ProjectPAULA
     cd C:\ProjectPAULA
     cmake -S . -B build -A Win32
     cmake --build build --config Release
     ```
     The `.exe` files and `libmysql.dll` land in the project folder.
   - **Prebuilt package (optional):** download `ProjectPAULA-v0.1.0-server-win32.zip` from the
     [release page](https://github.com/ceres7n/janus-emulator/releases) and unzip it to a short
     path such as `C:\ProjectPAULA`.
3. Open `install-db.bat` in a text editor and check the settings at the top: `MYSQL` (the path of
   `mysql.exe`), `DB_HOST`, `DB_PORT`, `DB_USER`, `DB_PASS` and `DB_NAME`. Save it and run it. It
   creates the database and all tables.
4. If your MariaDB uses another port, user, password or database name, put the same values into
   `db_port`, `db_user`, `db_pass` and `db_name` in all four files in `conf\`.
5. Create an account. `tools\add-account.bat` has its own copy of the database settings at its
   top: set the same values as in `install-db.bat`, then run `tools\add-account.bat myaccount`.
6. Start the servers: `run-server.bat`. Stop them later with `stop-server.bat`.
7. On the player side, get `PaulaLauncher.exe` and `launcher.ini` (in
   `ProjectPAULA-v0.1.0-launcher.zip` on the release page; a build from source makes
   `PaulaLauncher.exe` too, and `launcher.ini` is in `tools\launcher\`). Put both next to
   `Trickster.bin`, set `Account=myaccount` in `launcher.ini`, and run `PaulaLauncher.exe`.

## Documentation

| Document | For | Contents |
|---|---|---|
| [doc/INSTALL.md](doc/INSTALL.md) | Server operators | Full Windows install, public hosting, ports, accounts, updates, troubleshooting |
| [doc/CONFIGURATION.md](doc/CONFIGURATION.md) | Server operators | Every key of the four `conf\*.conf` files |
| [doc/GM_COMMANDS.md](doc/GM_COMMANDS.md) | Game masters | How to make a GM, all `@` commands, useful test ids |
| [doc/CONNECTING.md](doc/CONNECTING.md) | Players | Client folder, PaulaLauncher, first login, character creation |
| [doc/FEATURES.md](doc/FEATURES.md) | Everyone | What works, what is missing, known limitations |
| [doc/TESTING.md](doc/TESTING.md) | Beta testers | The beta test checklist |
| [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) | Developers | Processes, data flow, source layout, design rules |
| [CHANGELOG.md](CHANGELOG.md) | Everyone | Release notes |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Testers, developers | How to report bugs, development rules |
| [SECURITY.md](SECURITY.md) | Everyone | How to report a vulnerability |

## Important limits of this beta

- **There are no passwords in v0.1.0.** The account name is the only key: anyone who knows an
  account name can play that account. Give out account names that nobody can guess, and treat
  them like passwords. Unknown and banned accounts are refused.
- Never expose the MasterServer port (9010) or the database port to the internet.
- Some systems are missing or not yet tested with a real client. See
  [doc/FEATURES.md](doc/FEATURES.md).

## License

ProjectPAULA is licensed under the **GNU General Public License v3.0**. See [LICENSE](LICENSE).
Bundled third-party components and the game data are described in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
