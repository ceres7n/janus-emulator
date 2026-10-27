# Features and known limitations (v0.1.0)

This page lists every game system and its state in v0.1.0, then the limitations of the beta.

## Status legend

| Status | Meaning |
|---|---|
| **Tested live** | Played with the real client on a ProjectPAULA server |
| **Implemented** | A server-side tool; there is nothing to play |
| **Partial** | Some parts work; the note says what is missing |
| **Not implemented** | Not in this release |
| **Not supported** | Will not work with this release |

Beta testers and operators: [TESTING.md](TESTING.md) is a checklist to verify a server.

## A. Account, connection and saving

| # | System | What it does | Status |
|---|---|---|---|
| 1 | Account login | Log in with the account name from the launcher. Unknown and banned accounts are refused. | **Partial**: there is no password (see [Limitations](#accounts-and-security)) |
| 2 | Island select | After login, the island list shows "Ruby Island" | Tested live |
| 3 | Character list and selection | Character list with 3 slots per account | Tested live |
| 4 | Character creation | Race, hair, growth points (P/M/S/C) and name; starter items and skill | Tested live |
| 5 | Character deletion | Immediate, no waiting period; the name becomes free at once | Tested live |
| 6 | Community link (port 8003) | The client's second connection, used by party, whisper, friends and server-wide megaphones | Tested live |
| 7 | Saving | Every 15 seconds, on every map change and at logout; item changes at once | Tested live |
| 8 | Logout near other players | The others see the exit effect; your pet leaves with you and your buffs end | Tested live |

## B. World

| # | System | What it does | Status |
|---|---|---|---|
| 9 | Movement | Click to walk or run; walking is half the run speed | Tested live |
| 10 | Map change without relog | Portals and teleports move you to the next map without the login screen | Tested live |
| 11 | NPCs | Dialogs, menus, teleports and NPC scripts | Tested live |
| 12 | Monster spawns | Maps fill with monsters as in the original game | Tested live |
| 13 | Monster AI | Aggressive monsters attack on sight, chase and give up as in the original game | Tested live. Monsters move in straight lines (see [Limitations](#retail-behavior-kept-on-purpose)). |
| 14 | Shadow monsters and Third Eye | Shadow monsters only appear to, and only fight, players with Mind's Eye | Tested live |
| 15 | Monster speech | 68 monster kinds talk (in Korean, as in the original) when a fight starts or after a critical hit | Tested live |
| 16 | Sit and rest | Sitting or resting recovers HP and MP faster | Tested live |

## C. Combat and progress

| # | System | What it does | Status |
|---|---|---|---|
| 17 | Melee attack | Normal attacks with the original damage, critical, evade and block rules | Tested live |
| 18 | Class skills, 1st to 3rd job | 251 of the 305 original class skills are in. The rest need missing systems (for example couples or guardians) or have no code in the original server. | Tested live |
| 19 | Buffs and debuffs | Effects with icon and duration. Most buffs cannot be used in towns, as in the original. | Tested live |
| 20 | EXP, level and TM | EXP per hit, level up, TM on a kill. Level cap 400, TM level cap 500. | Tested live |
| 21 | Stat points | Spend free points in the status window | Tested live |
| 22 | Death, "Go to Town" and "Resurrect" | Death costs 2% of the current EXP. "Go to Town" returns you to the last town with 30% HP. Resurrecting on the spot is free up to level 20, then needs a resurrection item. | Tested live |
| 23 | Guns and ammunition | Guns use bullets; one shot per click | Tested live |
| 24 | Polar Bear line (Animal Lover, Trainer, Zoologist) | The 26 Polar Bear skills | Tested live. **Reconstructed**: no original server code exists for them. |
| 25 | Party skills | Auras, heals, party buffs and Bodyguard reach the party members nearby | Tested live |
| 26 | Skill masters | Job change (2nd and 3rd job) and skill mastery at the class's master | Tested live |
| 27 | Skill cards | Use a skill card item to learn its skill | Tested live |
| 28 | Skill scrolls | Scroll items that cast a skill | Tested live |
| 29 | Skills that need missing systems | Couple skills (5020-5041) and Guardian Summon (8003) do nothing. Five event skills (5008, 5011, 5013, 5015, 5043) do nothing, as on the original server. | Not implemented |

## D. Items and economy

| # | System | What it does | Status |
|---|---|---|---|
| 30 | Drops and pick-up | Monsters drop items and gelder; the drop's owner has priority for a while before others can pick it up | Tested live |
| 31 | Bag and rare items | Weight, stacks, up to 300 items. Rare items come with compound slots and random stats. | Tested live |
| 32 | Equipment | 23 slots; equipment changes your look and your stats | Tested live |
| 33 | Potions and discarding items | Use potions, throw items away | Tested live |
| 34 | Special use items | Working: gift boxes, warehouse slots, resurrection items, teleport items, resets, hair dye, disguise kits, rename (Reidentifier), type change (Befana's Unstable Elixir), Memory Port, megaphones. Missing: treasure maps, seeds, soul shards, guardian slot, rune stones, Star Tears, the Fiesta ticket and passive-skill seals (using them is refused). | **Partial** |
| 35 | Card items and throwing items | Using them is refused | Not implemented |
| 36 | Items with an expiry date (rental) | - | Not implemented |
| 37 | Pets | Equip a pet: it follows you and can help your recovery | Tested live |
| 38 | Pet speech | Pets talk (in Korean) in many situations | Tested live |
| 39 | NPC shops | Buy from and sell to NPCs | Tested live |
| 40 | Exchange shops | Trade items for items at NPCs | Tested live |
| 41 | Warehouse | Store items at the warehouse NPC (extra tabs with an item) | Tested live |
| 42 | Account bank | Items and gelder shared by the characters of one account | Tested live |
| 43 | NPC compound | Combine items at NPCs | Tested live |
| 44 | Refine, repair, mature compound, reset | - | Tested live |
| 45 | Drill | Dig with a drill to find buried items | Tested live |
| 46 | Crazy Drill | The Crazy Drill mode | Tested live |
| 47 | Memory Port | Save places and return to them with the item | Tested live |
| 48 | Megaphones | Mic: area message with a balloon. TBN Megaphone: a message to the whole server (one every 18 seconds). System megaphones from the server. | Tested live |

## E. Social and quests

| # | System | What it does | Status |
|---|---|---|---|
| 49 | Normal chat | Reaches the whole map (on this server a map is one area) | Tested live |
| 50 | Whisper | Private messages | Tested live |
| 51 | Chat rooms | - | Not implemented |
| 52 | Friends | List, groups, notes, a friend's position, going to a friend with the Friend Finder | Tested live |
| 53 | Party | Create, invite, EXP and TM share, item modes, party board | Tested live |
| 54 | Party quests | Kill quests for the whole party with a timer | Tested live |
| 55 | Normal and scenario quests | - | Tested live |
| 56 | Monster quests | - | Tested live |
| 57 | Trade between players | - | Tested live |
| 58 | Post box and level gifts | Mail with items and gelder, payment requests, level gifts | Tested live |
| 59 | Rename (Reidentifier) | The new name takes effect after a relog, as in the original | Tested live |
| 60 | Type change (Befana's Unstable Elixir) | Switches to the paired type of the same animal | Tested live |

## F. Missing or not supported

| # | System | Status |
|---|---|---|
| 61 | Guilds | Not implemented |
| 62 | Couples and marriage | Not implemented |
| 63 | MyCamp (player houses) | Not implemented |
| 64 | PvP, duels, guild wars | Not implemented |
| 65 | Hibernation (time capsule) | Not implemented |
| 66 | Non-ASCII character names (Korean and others) | Not supported: always refused |
| 67 | Rankings (drill, monster quest) | Not implemented |
| 68 | Tutorial quests | Not implemented |
| 69 | Mounts, Soul Shade (Guardian Summon) | Not implemented |
| 70 | Auto-drill and premium services | Not implemented |
| 71 | Premium and event EXP bonuses | Not implemented |
| 72 | Account passwords | Not implemented. Bans work (set in the database, see [INSTALL.md](INSTALL.md#82-ban-an-account)). |
| 73 | Reviving another player with an item | Not implemented |
| 74 | Server-wide chat | Does not exist in this client |
| 75 | Season 2 client | Not supported |

## G. Administration

| # | System | Status |
|---|---|---|
| 76 | GM commands ([GM_COMMANDS.md](GM_COMMANDS.md)) | Tested live |
| 77 | GM skills (GM's Support, GM's Dignity, Identification Card) | Tested live |
| 78 | GameServer crash dumps | Implemented: `GameServer_crash_<date>_<time>.dmp` |

## Limitations

### Accounts and security

- **No passwords.** The client sends no usable password, so v0.1.0 has none: the account name is
  the only key. Anyone who knows an account name can play that account. Operators should create
  account names nobody can guess and hand them out privately.
- Unknown account names and banned accounts are refused at login.
- A character selection is valid for one entry into the world, from the same IP address, within
  300 seconds. A player whose public IP address changes between connections is refused and must
  log in again.
- The MasterServer (port 9010) and the database must never be reachable from the internet
  ([INSTALL.md](INSTALL.md#10-public-hosting)).

### Retail behavior kept on purpose

ProjectPAULA copies the original server, including behavior that looks like a bug. Please do not
report these:

- Most buffs cannot be used in towns, indoor maps and minigames. The client shows no message.
- Receiving a letter that carries 2 or more items fails (error 114, the original database
  procedure) when any of them takes a new bag slot, that is, when it does not join a stack you
  already have. Letters with one item, letters whose items all join your stacks, and GM gifts
  with several items work.
- Bodyguard: the protected party member takes half the damage.
- In a party, the caster's buff icon of Sacred Oath, Refreshing Dew and the party scrolls may
  flicker.
- Monsters never say the "death" lines that exist in their data.
- In a "boss room" map, a pet does not talk if it was equipped there or if you logged in there.
- The party quest has no cancel button. When its time runs out it disappears without a message.
- The 3rd job announcement shows "JobTab_17" (and similar) instead of the job name.
- Level gifts stop once a character has taken the 2nd job.
- Firing a gun faster than the gun allows is ignored, and doing it 3 times in a row ends the
  connection (the original anti-cheat). Do not use macros for shooting.
- 41 NPCs stand at the point (600, 600) of their map, because their position is missing in the
  original data (the original server did the same).
- Monsters move in straight lines and do not walk around obstacles.
- With exactly 2 islands configured, the client shows its old two-world window without names.

### Reconstructed content

The original GameServer has no code for the **Polar Bear** line. Its 26 skills were rebuilt from
the client's classes and the game data. They work and were played, but details can differ from the
original game.

### Language of server texts

- NPC dialogs: mostly English (26,261 lines); 7,173 lines are still in the original Korean.
- Monster and pet speech, the level gift letters and the header of system megaphones
  (`[트릭스터]`): Korean, as in the original data.
- GM command answers: English or Portuguese.

### Other limits

- There are no rate settings: EXP, TM, gelder and drops are the original 1x values plus the
  original party bonus.
- A few requests the client sends are not handled yet, because their purpose is not identified.
  If something you do in the game is not saved, please report it with the time
  ([CONTRIBUTING.md](../CONTRIBUTING.md)).
- Addresses are IPv4 only.
- The GameServer runs its network work on a single thread in this release.
- A server overwrites its log when it starts; `run-server.bat` first moves the previous logs to
  `logs\` (see [INSTALL.md](INSTALL.md#11-logs-and-crash-dumps)).
