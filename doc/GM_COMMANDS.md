# GM commands

ProjectPAULA has 18 game master commands. A GM types them in the normal chat line, starting with
`@`. They are a ProjectPAULA addition: the original server had no such commands.

## Who is a GM

GM rights belong to a **character**, not to an account.

| Setting | Effect |
|---|---|
| `characters.authority` >= 1 | The character can use **all** `@` commands (there are no per-command levels) and wear GM-only items. |
| `characters.authority` = 2 | **Do not use.** 2 marks a deleted character: it disappears from the character list. Use 1. |
| `accounts.gm_level` > 0 | Does **not** give commands. The account only also sees islands marked "GM only" in the island list. |

To make a character a GM (create the character in the game first):

```sql
UPDATE characters SET authority = 1 WHERE name = 'CharacterName' AND deleted = 0;
```

The value is read when the character enters the game, so the player must log out and in again.
To remove the rights, set `authority = 0` the same way.

## How commands work

- Type the command in the normal chat. The line must **start with `@`**. Command names ignore
  upper and lower case; arguments are separated by spaces.
- A line that starts with `@` is never sent as chat, also not for normal players.
- A character without GM rights gets no answer: the command is ignored and only written to the
  server log.
- Answers appear as a green system line in the chat. `@jump`, `@heal`, `@kill` and `@save` give no
  answer, and `@warp` answers only when it fails. `@item`, `@warp`, `@skill`, `@spawn` and
  `@sysmega` without their arguments answer with their usage, for example
  `usage: @item <id> [amount]`.
- An unknown command answers `unknown command: @<name>`.
- Every GM command is written to `game_live.log` at the `info` level, with the character and the
  full command line. A refused attempt by a character without GM rights is logged too.
- A few answers are still in Portuguese (fixed texts in the server code): `@cdm` (`ativo por N s`
  = "active for N s"), `@speed` (`correr` = run, `andar` = walk) and `@blvl` (`pts livres` = free
  points). All other answers are in English.
- Ids come from the data files: items from `db/item_db.yml`, maps from `db/maps_db.yml`,
  monsters from `db/mob_db.yml`, skills from `db/skill_db.yml`.

## Command reference

| Command | Syntax | What it does | Example |
|---|---|---|---|
| `@item` | `@item <id> [count]` | Puts the item **straight into your bag**, without a weight check. Count defaults to 1 (at most 65535). An item that is not a common item (a rare item, for example) always comes as 1; a new drill comes with its drill life. Refused if the id does not exist (`@item: item N does not exist (item_db.yml)`), or if the bag already holds 300 entries and none of them is a stack the item can join (`@item: the bag is full (300)`). Answer: `@item: <name> xN`. | `@item 19656 5` |
| `@warp` | `@warp <map> [x y]` | Moves you to another map without a relog, like a portal. Without coordinates you arrive at the map's arrival point. **The coordinates are not checked**: a point inside a wall or outside the map can leave you stuck; then `@warp <map>` without coordinates. Answer on a bad map id: `@warp: map N does not exist (maps_db.yml)`. | `@warp 148`, `@warp 1 3200 3560` |
| `@jump` | `@jump [x y]` | **Debug only.** Changes your position on the server only; the client is not told, so your character does not move and gets out of sync. Use `@warp <current map> x y` instead. | - |
| `@where` | `@where` | Shows your map and position. Answer: `@where: zone=148 (949,2228)`. | `@where` |
| `@heal` | `@heal` | Sets HP and MP to their maximum on the server. Your bars update with the next recovery tick (up to 2 seconds). | `@heal` |
| `@kill` | `@kill` | **Debug only.** Sets your HP to 0 on the server **without** killing you: no death screen, and the next recovery tick gives HP back. | - |
| `@save` | `@save` | Saves your character now (map, position, gelder, level, EXP, HP, MP, stat points). | `@save` |
| `@speed` | `@speed [n]` | Sets your run speed to `n` (4 to 1020); walking is always half of it. Without `n`: back to the normal 160. **Lasts until your next map change or relog.** | `@speed 320` |
| `@blvl` | `@blvl <+N or -N>` | Changes your base level by N (limits 1 to 400) and sets EXP to 0. **Up** is a real level-up: stats, +4 free points per level, full heal, the level-up effect, **the level gifts of every level passed**, and at level 400 **an announcement to the whole server**. **Down** recalculates your points from your growth. Saves. | `@blvl 9` |
| `@tmlvl` | `@tmlvl <+N or -N>` | Changes your TM level by N (0 to 500), sets TM EXP to 0, and gives or takes 1 TM point per level (never below 0). Saves. | `@tmlvl 50` |
| `@skill` | `@skill <id> [level]` | Learns a skill or sets its level (default 1, capped at the skill's maximum). Level 0 removes it. **Log out and in to see it** in the skill window. | `@skill 5019` |
| `@spawn` | `@spawn mob [id] [name]` or `@spawn npc <id> [name]` | Creates a monster (with its real stats from `db/mob_db.yml`) or an NPC model at your position. Without an id, `mob` creates monster 2183 (Torobbie, level 1). Aggressive monsters attack. | `@spawn mob 2053` |
| `@levelgift` | `@levelgift [level]` | With a level: sends that level's level gift to your post box. Without: checks your post box now. | `@levelgift 15` |
| `@sysmega` | `@sysmega <text>` | A system megaphone to the whole server: `[트릭스터] <megaphone_island_name> - <text>`. It joins the megaphone queue (one every 18 seconds). Only players whose community link (port 8003) is connected see it. | `@sysmega Maintenance in 10 minutes` |
| `@pqkill` | `@pqkill [n]` | Test helper: adds n kills (default 1, at most 500) of the running party quest's monster for you. Answer: `@pqkill: done` or `@pqkill: no party quest running`. | `@pqkill 85` |
| `@quest` | `@quest [quest id]` | Without an id: lists your normal quests in progress (round, items present or missing). With an id: the quest's MaxCount and how often you finished it, with the date. | `@quest`, `@quest 3005` |
| `@dig` | `@dig` | Drill information for the cell you stand on (ground type, buried item and depth), the buried items of the map, and the Crazy Drill state. | `@dig` |
| `@cdm` | `@cdm` | Starts Crazy Drill Mode now. Needs skill 5019 (`@skill 5019`). Answer: `@cdm: Crazy Drill ativo por N s` ("active for N s"). | `@cdm` |

## GM skills

These skills exist in the data and work on the server, but have **not been tested with a real
client** yet. Learn them with `@skill <id>`, then log out and in.

| Skill id | Name | Effect |
|---|---|---|
| 20000 / 20001 | GM's Support (Attack / Defense) | A buff on the selected target, player or monster |
| 20002 / 20003 | GM's Dignity (Attack / Defense) | An area buff at a point: the players inside the radius |
| 8002 | Identification Card | Opens the identification window |

## Warnings

- `@item` skips the weight check. Items a GM creates are normal items: they can be traded, sold
  and mailed like any other item.
- `@blvl` on a low character sends many level gifts at once, and reaching level 400 is announced
  to every player.
- `@sysmega` reaches every connected player. The name in the middle comes from
  `megaphone_island_name` in `conf\game_server.conf` (default `Paula`).

## Ids useful for testing

Names as in the server data.

**Items**

| Id | Item |
|---|---|
| 19512 | Friend Finder (go to a friend from the friend list) |
| 19655 | Mic (area megaphone) |
| 19656 | TBN Megaphone (server-wide megaphone) |
| 19659 | Memory Port |
| 19647 | Reidentifier 2000 (rename) |
| 19616 | Befana's Unstable Elixir (change type) |
| 1762 | Test Dragon Pet (talks in almost every situation) |
| 15800 | Little Diablo (pet) |

**Maps**

| Id | Map |
|---|---|
| 148 | Coral Town - Blooming Cora (where new characters start) |
| 1 | Megalopolis Square |
| 35 | Paradise Monster Guild (Officer Tera, party quest 3005) |
| 13 | Beach Field 3 - Statue of Anubis (Pineapples) |
| 234, 235 | GM Garden of Chaos |

**Monsters**

| Id | Monster | Level |
|---|---|---|
| 2183 | Torobbie | 1 |
| 2190 | Bluffy Bad Puri | 9 |
| 2035 | Golden Mole | 20 |
| 2053 | Pineapple | 24 |
| 2061 | Grumpy Octopus | 29 |
| 2015 | Electric Snail | 164 (kills a low-level character at once) |
