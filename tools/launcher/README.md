# PaulaLauncher

`PaulaLauncher.exe` starts the Trickster Online KR 2014 client (`Trickster.bin`) and points it
at a ProjectPAULA server. The client has its server address baked in, so the launcher replaces
that address in the running client's memory with the address from `launcher.ini`.
Nothing on disk is changed.

## For players

1. Put `PaulaLauncher.exe` and `launcher.ini` in your client folder, next to `Trickster.bin`.
2. Edit `launcher.ini`:
   - `LoginIP`: the server's IPv4 address (digits and dots, no host names).
   - `Account`: your account name (1-18 letters, digits or `_`), as the server operator
     created it.
3. Start `PaulaLauncher.exe`, or `PaulaLauncher.exe <account>` to use another account.

The launcher prints what it did. On an error it explains the problem; when it has its own console window
(started by double-click) it also waits for a key before closing.

| Key | Default | Notes |
|---|---|---|
| `[Launcher] TricksterPath` | `Trickster.bin` | A relative path is relative to the launcher's folder. |
| `[Launcher] OriginalIP` | `217.160.132.225,51.38.50.68,211.44.251.24` | The IP baked into the client; a comma-separated list. The default covers all known builds. |
| `[Server] LoginIP` | `127.0.0.1` | The server's IPv4 address, at most 15 characters. |
| `[Server] LoginPort` | `9980` | Only shown: the supported clients always connect to port 9980. |
| `[Server] Account` | (none) | Required, here or as the first argument. |

Exit codes: 0 = the client is running and points at the server; 1 = invalid setting;
2 = client not found; 3 = the client could not start, or it closed right away; 4 = the server IP
could not be put into the client (the launcher then closes the client, so it does not connect
to another server).

## How it works

1. It starts `Trickster.bin` with the command line
   `"<LoginIP>,<Account>,0,Trickster,0,blankblank0|0"`, the format the retail web launcher used.
2. After 3 seconds it scans the client's readable memory for each `OriginalIP` and rewrites
   every copy with `LoginIP`. A copy is only rewritten where the new address fits: past the
   end of the old address, only bytes that are already NUL padding may be overwritten.
   The client's IP table uses 16-byte slots, so any IPv4 address fits there. A copy that is
   too short is left unchanged and reported.
3. If no copy was found or none could be rewritten, it closes the client.

Antivirus software may warn about a program that writes into another process's memory.
If the client folder needs administrator rights, the launcher needs them too.

## Building

The launcher is part of the main CMake build (target `PaulaLauncher`, see the repository's
`doc/INSTALL.md`). It is a 32-bit console program with the C runtime linked in statically, so it
needs only `KERNEL32.dll` and no Visual C++ redistributable. Like the servers, it lands in the
project root.
