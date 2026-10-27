# Janus Launcher

`JanusLauncher.exe` starts the Trickster Online KR 2014 client (`Trickster.bin`) and points it
at a ProjectPAULA server. The client has its server address built in, so the launcher puts the
server's address into the client's memory before the client runs. Nothing on disk is changed.

## For players

1. Put `JanusLauncher.exe` (and, if you like, `JanusLauncher.ini`) in your client folder, next
   to `Trickster.bin`.
2. Start `JanusLauncher.exe`.
3. Type the **Server address** the server operator gave you (an IPv4 address such as
   `203.0.113.7`, or a host name). Keep `127.0.0.1` if the server runs on your own PC.
4. Type your **Account** name (1-18 letters, digits or `_`), as the server operator created it.
5. Press **Play** (or Enter).

The **Status** box says what happened: the client build it found, the client's process id, how
many copies of the built-in address it replaced, and, on an error, what to do next. The same
lines go to `JanusLauncher.log` next to the launcher (or to your `%TEMP%` folder if that folder is
read-only); send that file to the server operator when something goes wrong. Each start of the
launcher begins a new log; the one before is kept as `JanusLauncher.log.1`.

The address and account are saved in `JanusLauncher.ini` each time the game starts. Press Play
again to start another client, for example with a second account (the Client_Janus build allows
5 clients at a time, the reference build 3). Tick "Close this window when the game starts" if you
do not need the launcher afterwards.

**Advanced** (most players never need it):

| Field | Default | Notes |
|---|---|---|
| Client | `Trickster.bin` | A relative path is relative to the launcher's folder. **Browse...** picks the file. |
| Replace IPs | `51.38.50.68,217.160.132.225,211.44.251.24` | The server addresses built into the client builds; up to 8. |
| Session key | `0` | 1-18 digits. **Not a password**: the client sends it as the password text and the server ignores it. Never type a real password here: it ends up in the command line, the ini and the log. |
| Age | `0` | 0-255. Only the Korean youth curfew reads it, and ProjectPAULA never enables that. |
| Game ID | `Trickster` | Fixed: the client accepts nothing else. |
| Extras | `blankblank0\|0` | Letters, digits and `_ . - \|`. Must not start with `devsit` and must not have a 4th `\|` value other than 0 (the client would refuse to start or close itself at that hour). |

**Restore defaults** puts back the session key, age and extras. **Command line** shows exactly
what the client gets. The login port is 9980; it is built into the client and cannot be changed.

## Command line

```
JanusLauncher.exe                    the window
JanusLauncher.exe <account>          the window, with this account filled in
JanusLauncher.exe --play [options]   no window: start one client and exit
JanusLauncher.exe --selftest         run the built-in tests (exit code 0 = all passed)
JanusLauncher.exe --help             usage
```

Options of `--play` (each also as `--name=value`); a value not given comes from
`JanusLauncher.ini`, then from the defaults. `--play` never changes the ini.

| Option | Ini key | Default |
|---|---|---|
| `--ip <address>` (or `--address`) | `[Server] Address` | `127.0.0.1` |
| `--account <name>` | `[Server] Account` | none (required) |
| `--client <path>` | `[Launcher] ClientPath` | `Trickster.bin` |
| `--replace-ips <list>` | `[Launcher] ReplaceIPs` | `51.38.50.68,217.160.132.225,211.44.251.24` |
| `--key <digits>` (or `--session-key`) | `[Launcher] SessionKey` | `0` |
| `--age <0-255>` | `[Launcher] Age` | `0` |
| `--extras <text>` | `[Launcher] Extras` | `blankblank0\|0` |
| (window only) | `[Launcher] CloseOnStart` | `0` |

Started from a console, `--play`, `--selftest` and `--help` print there (output redirected to a
file or a pipe works too). The launcher is a Windows program, so `cmd` does not wait for it: use
`start /wait JanusLauncher.exe --play ...` and then `echo %ERRORLEVEL%`, or PowerShell's
`(Start-Process JanusLauncher.exe '--play','--account','name' -Wait -PassThru).ExitCode`.

Exit codes of `--play`:

| Code | Meaning |
|---|---|
| 0 | The client runs and points at the server. |
| 1 | A setting is not valid (or the host name was not found). |
| 2 | The client was not found. |
| 3 | The client could not start, or it closed right away (the log says why). |
| 4 | The server address could not be put into the client. The client is closed before it runs, so it never connects anywhere. A 64-bit program chosen as the client also gives 4; it is not started at all. |

## How it works

1. Every value is checked first. A host name is looked up (IPv4 only). The client build is
   identified by the MD5 of `Trickster.bin`: the Client_Janus build, the reference build, or
   unknown (allowed, with a note).
2. The client is created **suspended**, with its own folder as working directory and the command
   line `"<client>" "<IPv4>,<account>,<key>,Trickster,<age>,<extras>"`, the format of the retail
   web launcher (live-tested). The first field is not used for the connection.
3. While none of the client's code has run, every copy of each Replace IP inside the client's
   program image is replaced with the server's IPv4 address. The client reads its server address
   from a table of 18 slots of 16 bytes; a copy is only rewritten where the new address fits
   (past the old address, only bytes that are already zero padding are overwritten). For the two
   known builds the launcher then checks that all 18 slots hold the new address.
4. If no copy was found, a copy could not be rewritten, or the slot check fails, the client is
   ended without ever running (exit code 4). Otherwise it is resumed.
5. About 3 seconds later the launcher scans all of the client's memory once more for leftover
   copies of the old addresses (expected: none) and reports the count. If the client has
   already closed, it reports the exit code and the likely reason (too many clients open, a
   missing DLL, a crash).

The launcher only ever opens the client it just started. Packed client builds, such as the KR
January 2014 client, are not supported: their address is not readable before they run.

Antivirus software may warn about a program that starts another program suspended and writes
into its memory, which is exactly what this launcher has to do. If the client folder needs
administrator rights, the launcher needs them too.

## Building

The launcher is part of the main CMake build (target `JanusLauncher`, see the repository's
`doc/INSTALL.md`); like the servers it lands in the project root. It also builds on its own:

```
cmake -S tools/janus-launcher -B build-launcher -A Win32
cmake --build build-launcher --config Release
```

It is a 32-bit Windows program with the C runtime linked in statically, so it needs only Windows
system DLLs and no Visual C++ redistributable. The window, the version information and the
manifest (no elevation, visual styles, per-monitor DPI awareness, Windows 7 to 11) are in
`janus_launcher.rc` and `JanusLauncher.manifest`.
