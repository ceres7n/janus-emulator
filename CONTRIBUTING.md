# Contributing to ProjectPAULA

Thank you for helping. There are two ways to contribute: report bugs from the beta, and send code.

**Security problems are not reported here.** Follow [SECURITY.md](SECURITY.md) instead.

## Reporting a bug

### Before you report

- Check [doc/FEATURES.md](doc/FEATURES.md). A system may be listed as not implemented, and some
  odd behavior is the original game's and is kept on purpose ("Retail behavior kept on purpose").
- Check that you use the supported client build ([doc/CONNECTING.md](doc/CONNECTING.md#1-your-client-folder)).

### What to write

Open an issue in the repository's issue tracker
(<https://github.com/ceres7n/janus-emulator/issues>) with:

| Field | Example |
|---|---|
| Date and time, **with your time zone** | 2026-10-27 21:14 UTC-3 |
| Server version | v0.1.0 (each server prints it when it starts) |
| Character name and race/job | Pumpkin, Lion / Inventor |
| Map and position | Coral Town; GMs can type `@where` |
| What you did | Step by step |
| What you expected | |
| What happened instead | Exact message texts, if any |
| Screenshot or video | If it helps |

The time matters most: the operator finds the matching lines in the server logs by time.

### What to attach

**Players**, when the client crashed or closed:
- the newest `COD_*.txt` file from the `capture` folder of your client.

**Operators** (or a player, through their operator):
- the server logs `game_live.log`, `login_live.log`, `auth_live.log` and `master_live.log` of the
  run where the problem happened. `run-server.bat` moves the logs of the previous run to `logs\`
  (named `<date>_<time>_<name>_live.log`) before it starts the servers again;
- after a GameServer crash: the `GameServer_crash_<date>_<time>.dmp` file, and which build
  produced the crashed `GameServer.exe` (the release version for a prebuilt package, or the
  commit you built). Only the `GameServer.pdb` of that same build can read the dump.

Important:
- **Copy the logs before you restart a server by hand.** A server started without
  `run-server.bat` overwrites its log when it starts.
- Logs contain account names, character names and IP addresses. Remove what you do not want to
  publish, or send the logs to the maintainers privately.
- Never attach your `conf\*.conf` files with the database password in them.

## Contributing code

### Ground rules

1. **1:1 with retail.** ProjectPAULA reproduces the original server, including its quirks. Do not
   "fix" original behavior, change packet layouts or opcodes, retune formulas, rates or timings,
   or change anything an unmodified client would notice, unless the original binary shows that
   the current code is wrong.
2. **Keep the provenance comments.** Comments that cite `sub_XXXXXX`, `0xADDR`, "retail" or "1:1"
   say where a rule comes from. Keep them, and add them to new code.
3. **Verify every S2C packet in the client.** Send an opcode only after its number and its body
   layout were checked in the client binary. Verify the cause of a bug the same way before
   fixing it.
4. **Port stored procedures 1:1.** A database feature starts from the original server's stored
   procedure, ported line by line into a C++ method (store classes in `src/common/store/` or the
   GameServer module). Do not add stored procedures to the database.
5. **Game data in `db/*.yml`:** keep the common layout (comment header, then `Header` / `Body` /
   `Footer`), CamelCase field names as in the original columns, and **numeric ids** (no names as
   keys). Name the source of the data in the file header.
6. **Mark hardening.** A server-side check that the original server did not have, added because
   a modified client could abuse its absence, starts with a comment
   `// Hardening (not retail):` followed by one line: what retail does, and why we check.
   It must not change anything for an unmodified client.
7. **Logging:** `INFO` is for lifecycle events, connections, logins, character enter and leave,
   map changes, GM command use, refused requests and economy audit lines. Per-packet, per-move,
   per-hit and per-tick detail is `DEBUG`.

### Code style and build

- Match the style, naming and comment density of the surrounding code. Do not reformat whole
  files, and keep each file's line endings (`.gitattributes` defines them; `.bat` files are CRLF).
- The build is **32-bit only**: `cmake -S . -B build -A Win32`, then
  `cmake --build build --config Release` ([doc/INSTALL.md](doc/INSTALL.md#32-build-from-source)).
- A change must build with **0 errors and no new warnings** at the project's warning level (`/W3`).

### Pull requests

- One topic per pull request.
- Describe what you changed, why, and how you tested it, ideally with the supported client.
- Name the retail evidence (function, address or data table) for any gameplay change.
- By contributing you agree that your contribution is licensed under the
  [GNU General Public License v3.0](LICENSE), like the rest of ProjectPAULA.
