# Changelog

All notable changes to ProjectPAULA are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the version numbers follow
[Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-10-26

The first public beta.

### Added

- **Server stack:** `AuthServer` (account login and island list, TCP 9980), `LoginServer`
  (character select, TCP 8001), `MasterServer` (character data service, TCP 9010, internal) and
  `GameServer` (the world, TCP 8002, plus the community link on TCP 8003 for party, whisper,
  friends and megaphones). The LoginServer reconnects to the MasterServer by itself.
- **Source and packages:** <https://github.com/ceres7n/janus-emulator>, with the optional prebuilt
  packages `ProjectPAULA-v0.1.0-server-win32.zip` and `ProjectPAULA-v0.1.0-launcher.zip` on the
  release page.
- **PaulaLauncher** (`tools/launcher/`): starts the player's `Trickster.bin` and points it at a
  ProjectPAULA server through `launcher.ini` (`LoginIP`, `Account`). It covers all known client
  builds, never writes past the original address in the client's memory, and explains every error.
- **Scripts:** `run-server.bat` (checks the files, starts the servers in order and keeps the
  previous logs in `logs\`), `stop-server.bat` (lets the GameServer save first), `install-db.bat`
  and `tools/add-account.bat`.
- **Database scripts** `sql/001` to `sql/019`: a complete, re-runnable schema. No default
  accounts are created.
- **Game systems**, ported 1:1 from the retail server (status of each one in
  [doc/FEATURES.md](doc/FEATURES.md)):
  - character creation with the original starter kit and name rules, deletion, 3 slots per account;
  - seamless map changes through portals and teleports, NPC dialogs and scripts, monster spawns,
    monster AI, monster speech;
  - melee and gun combat, class skills of all races up to the 3rd job, effect states (buffs and
    debuffs), party skills, skill cards and scrolls, skill masters and job changes;
  - EXP, TM, level caps 400 and TM 500, death, "Go to Town" and revive;
  - drops, bag, rare items, 23 equipment slots, item use, pets and pet speech, NPC shops,
    exchange shops, warehouse, account bank, compound, refine and mature compound, drill and
    Crazy Drill, Memory Port, megaphones;
  - chat, whisper, friends, party, party quests, normal, scenario and monster quests, post box and
    level gifts, trade between players, rename and type change items.
- **The Polar Bear line** (26 skills), reconstructed from the client and the game data: the
  original server has no code for it.
- **18 GM commands** ([doc/GM_COMMANDS.md](doc/GM_COMMANDS.md)).
- **Saving:** every 15 seconds, on every map change, at logout, and when the GameServer stops,
  including when its console window is closed.
- **Crash dumps:** `GameServer_crash_<date>_<time>.dmp`.
- **Configuration keys** `log_level` (all servers, default `info`) and `bind_ip` (all servers);
  `game_ip` is now in the shipped `conf/game_server.conf`.
- **Documentation:** README, install guide, player guide, configuration reference, GM commands,
  features and limitations, architecture, beta test checklist, contributing and security notes.

### Security

- Unknown and banned accounts are refused at login. There is still **no password** in this beta:
  the account name is the key (see [doc/FEATURES.md](doc/FEATURES.md#accounts-and-security)).
- The GameServer admits a character only with a one-time ticket that the LoginServer issues on
  character select, bound to the character, its account and the player's IP address, and valid
  for 300 seconds.
- A second login of a character that is still online saves and closes the old session first.
- The MasterServer listens on `127.0.0.1` by default. A second copy of a server can no longer
  share a port with the first one.
- The server now checks requests that only a modified client could send. Each such check is
  marked `// Hardening (not retail):` in the code. An unmodified client sees no difference.

### Known issues

- Trade, the post box and the logout effect failed in a live test, were fixed, and await a
  retest.
- Not implemented: guilds, couples, MyCamp, PvP, chat rooms, card and throwing items, items with
  an expiry date, rankings, tutorial quests, account passwords.
- The full list of limitations: [doc/FEATURES.md](doc/FEATURES.md#limitations).

[0.1.0]: https://github.com/ceres7n/janus-emulator/releases/tag/v0.1.0

