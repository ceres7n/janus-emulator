# Connecting to a ProjectPAULA server (player guide)

This guide is for players. You need three things:

1. **A Trickster Online KR 2014 client folder.** ProjectPAULA does not distribute the game client:
   you build your own client folder (section 1).
2. **`PaulaLauncher.exe` and `launcher.ini`** from the ProjectPAULA release:
   `ProjectPAULA-v0.1.0-launcher.zip` on the
   [release page](https://github.com/ceres7n/janus-emulator/releases), or from your server
   operator.
3. **From the server operator:** the server's IPv4 address and your account name.

Contents:
1. [Your client folder](#1-your-client-folder)
2. [Install PaulaLauncher](#2-install-paulalauncher)
3. [Start the game](#3-start-the-game)
4. [Create a character](#4-create-a-character)
5. [Your first steps](#5-your-first-steps)
6. [Troubleshooting](#6-troubleshooting)

## 1. Your client folder

ProjectPAULA was developed and tested with **one client build**. Use a client folder whose
`Trickster.bin` is exactly this file:

| Property | Value |
|---|---|
| File | `Trickster.bin` |
| Size | 14,335,488 bytes |
| MD5 | `46ee494cb3042cdf93c1f9b42f1e16a4` |

To check your file, open PowerShell in the client folder and run:

```
(Get-Item .\Trickster.bin).Length
Get-FileHash .\Trickster.bin -Algorithm MD5
```

Other builds:

| Client | Status |
|---|---|
| The reference build above | Supported. ProjectPAULA was developed and live-tested with it. |
| The KR client of January 2014 (`Trickster_KRN_012014_Client`) | **Not supported.** It logs in, but crashes in the game world. |
| Any other build | Not tested. It may work if it uses the same network protocol. |

## 2. Install PaulaLauncher

The client has a server address built into it. `PaulaLauncher.exe` starts the client and replaces
that address in the running client's memory with the address of your server. Nothing on disk is
changed.

1. Copy `PaulaLauncher.exe` and `launcher.ini` (unzipped from `ProjectPAULA-v0.1.0-launcher.zip`)
   into your client folder, **next to `Trickster.bin`**. The launcher needs no extra runtime.
2. Open `launcher.ini` with a text editor and set two values:
   ```ini
   [Server]
   LoginIP=203.0.113.25
   Account=youraccount
   ```
   - `LoginIP`: the server's IPv4 address from the operator. Digits and dots only (at most 15
     characters), no host names. For a server on your own PC keep `127.0.0.1`.
   - `Account`: your account name, exactly as the operator created it (1 to 18 letters, digits
     or `_`).
3. Save the file.

All keys of `launcher.ini`:

| Section and key | Default | Meaning |
|---|---|---|
| `[Launcher] TricksterPath` | `Trickster.bin` | The client to start. A relative path is relative to the launcher's folder. |
| `[Launcher] OriginalIP` | `217.160.132.225,51.38.50.68,211.44.251.24` | The address built into your client. The default covers all known builds; change it only if the launcher says it found no address. A comma-separated list is allowed. |
| `[Server] LoginIP` | `127.0.0.1` | The server's IPv4 address |
| `[Server] LoginPort` | `9980` | Shown only. These clients always connect to port 9980. |
| `[Server] Account` | (none) | Your account name. Required, here or on the command line. |

In `launcher.ini`, a line that starts with `;` is a comment. Keep comments on their own lines:
text after a value becomes part of the value.

## 3. Start the game

1. Run `PaulaLauncher.exe`. To use another account without editing the file, run
   `PaulaLauncher.exe <account>` from a command prompt.
2. The launcher starts the client, waits a few seconds, points it at the server and prints what it
   did. If something is wrong, it explains the problem (section 6); when you started it by
   double-click, its window stays open until you press a key.
3. The client logs in with your account name. **There is no password to type**: in this beta the
   account name is your key, so keep it private.
4. The island list appears. Choose **Ruby Island** (click it, or press `1` or Enter).
5. The character list appears. Each account has **3 character slots**. Create a character
   (section 4) or select one.
6. You enter the world. Once connected, the window title shows "Trickster Paula".

## 4. Create a character

### 4.1 Race

| Race | First job | Second job |
|---|---|---|
| Bunny | School Girl | Boxer |
| Buffalo | Fighter | Warrior |
| Sheep | Librarian | Bard |
| Dragon | Shaman | Magician |
| Fox | Archaeologist | Explorer |
| Lion | Engineer | Inventor |
| Cat | Model | Talent |
| Raccoon | Teacher | Card Master |
| Polar Bear | Animal Lover | Trainer |

Your client shows the names from its own data files, so they can differ slightly. The second and
third jobs are taken at your class's skill master in the game.

The Polar Bear's 26 skills are a **reconstruction**: no original server code exists for them. See
[FEATURES.md](https://github.com/ceres7n/janus-emulator/blob/main/doc/FEATURES.md).

### 4.2 Growth points

You split **10 points** over the four growth types **P** (Power), **M** (Magic), **S** (Sense) and
**C** (Charm):
- each type gets 1 to 4 points;
- the type you choose as your main type must get 4.

The main type also decides your first skill (section 5).

### 4.3 Name rules

| Rule | Details |
|---|---|
| Length | 4 to 16 characters |
| Characters | Letters `A-Z` and `a-z`, digits `0-9`, and `^`. No spaces. |
| Refused characters | `_` (refused anywhere in the name), Korean letters, accented letters and other non-ASCII characters |
| Refused words | Names that contain certain words anywhere, in any case, are refused. The list includes `admin`, `gm`, `gamemaster`, `trickster`, `sex`, `anal`, `gay`, `damn`, `dummy` and `porno`. `g?m` (a `g`, any one character, then `m`) is also refused. |
| Unique | Upper and lower case count as the same name: "Kiwi" and "kiwi" cannot both exist. |

Names refused because of the word list, as examples: **Sigma**, **Magma** (contain `gm`),
**Gamer**, **Gemma**, **Gym** (`g?m`), **Essex** (`sex`), **Canal** (`anal`).

When a name is refused, the client shows an error (invalid name, or name already in use).
Deleting a character takes effect at once (there is no waiting period) and frees its name.

## 5. Your first steps

**Where you start.** Every new character starts in **Coral Town - Blooming Cora** at level 1, with
30% HP, 0 MP and 0 gelder. Coral Town is also your first return town.

**Starter items:**
- the ears and tail of your race (worn);
- 30 Lucky Pink Potion and 30 Lucky Blue Potion;
- the Duckling set: Duckling Staff (the Polar Bear gets a Duckling Hammer instead), Duckling Hat,
  Duckling Shield and Duckling Keychain;
- Polar Bear only: Beginner Hammer, Puchi Bear, Neo hammer of Hunting and Strong Ring.

**Starter skill**, by your main growth type:

| Main type | Skill |
|---|---|
| Power | Steel Punch |
| Magic | Magical Soul |
| Sense | Stone Shower |
| Charm | Warm Up |
| Polar Bear (any type) | Equip Hammer and Powerful Hammer |

**Progress.** EXP comes with each hit; TM comes when you kill a monster. The highest level is 400
and the highest TM level is 500. Rates are the original 1x rates, plus the original party bonus.
The bag holds up to 300 items.

**Level gifts.** At levels 15, 20, 25, 31, 40, 60, 80, 100, 120, 135, 140, 160, 180 and 200 a gift
arrives in your post box (gift tab). As in the original game, only characters that have not taken
their second job receive them, and the gift letters are in Korean.

**Death:**
- You lose 2% of your current EXP.
- A unique item worn in one of the first three equipment slots has a 1 in 5 chance to come off;
  it goes to your bag.
- **Go to Town** takes you to the last town you visited, with 30% HP. Up to level 20 the lost EXP
  is given back.
- **Resurrect** (stand up where you died) is free up to level 20. Above level 20 it uses up a
  resurrection item from your bag; without one it is refused.

**Buffs in towns.** As in the original game, most buffs cannot be used in towns, indoor maps and
minigames. The client shows no message when this happens.

**Server text languages.** Most NPC dialogs are in English; some are still in the original Korean.
Monster and pet speech, level gift letters and the header of system megaphones (`[트릭스터]`) are in
Korean, as in the original data.

**The community link.** Your client opens a second connection to the server for party, whisper,
friends and server-wide megaphones. If it cannot connect, the client says "Party system is not
available due to disconnection from the Server" and retries every few seconds. Everything else
keeps working.

What works and what is missing: [FEATURES.md](https://github.com/ceres7n/janus-emulator/blob/main/doc/FEATURES.md).

## 6. Troubleshooting

| Problem | What to do |
|---|---|
| The launcher says it cannot find `Trickster.bin` | Put `PaulaLauncher.exe` and `launcher.ini` in the client folder, or set `TricksterPath` to the full path of `Trickster.bin`. |
| The launcher says it found no server address in the client, and closes the client | Your client build is not a known one. Check it against section 1. If you know the address built into your client, add it to `OriginalIP`. |
| The launcher reports an invalid setting | `LoginIP` and each `OriginalIP` entry must be a numeric IPv4 address, `LoginPort` a number from 1 to 65535, and `Account` must be set (1 to 18 letters, digits or `_`). |
| A "LOGIN ERROR" message box saying the login failed (in Korean clients: "로그인에 실패했습니다...") | The account name is unknown or the account is banned. Check `Account` for typos and ask the operator. |
| The client cannot connect at all | Check `LoginIP`. The server may be down, or a firewall may block port 9980. |
| You are disconnected while entering the world | Log in again. The server ties your character selection to your IP address for a few minutes; if your connection changed its IP address in between, the server refuses the entry. |
| "Party system is not available due to disconnection from the Server" | The community link (port 8003) cannot connect. Tell the operator; the rest of the game keeps working. |
| Your character is stuck or jumps back when moving | Log out and in again, then report it (with the time and the map) to the operator. |
| You log in again after a client crash and your character was still online | The server saves and closes the old session first, then lets you in. |
| The client crashes | Send the operator the time, what you were doing, and the newest `COD_*.txt` file from the `capture` folder of your client. |
| Your antivirus warns about `PaulaLauncher.exe` | The launcher writes into the client's memory to change the server address, which some antivirus programs flag. |
| The launcher cannot start the client | If your client folder needs administrator rights, run the launcher as administrator too. |
| The client crashes in the world right after login | Check your client build (section 1). The KR client of January 2014 is not supported. |
