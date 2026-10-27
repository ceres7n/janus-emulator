# Beta test checklist (v0.1.0)

This checklist is for beta testers and operators who want to verify a server. Every system in it
passed in game for v0.1.0; it starts with what changed most in this release, then covers the rest
of the game for regression checks.

## Before you start

- **Accounts:** two accounts (A and B) for the tests that need two players. Two clients can run on
  one PC.
- **A GM character:** most preparations use GM commands. Ask the operator to set
  `authority = 1` on a test character ([GM_COMMANDS.md](GM_COMMANDS.md)), or run your own test
  server ([INSTALL.md](INSTALL.md)).
- **Write down the time** (with your time zone) of everything that goes wrong. Operators find the
  matching lines in the server logs by time.
- **Report** each problem as described in [CONTRIBUTING.md](../CONTRIBUTING.md). Behavior listed
  under "Retail behavior kept on purpose" in [FEATURES.md](FEATURES.md#retail-behavior-kept-on-purpose)
  is not a bug.

Quick reference:

| Need | Command |
|---|---|
| An item | `@item <id> [count]` |
| A monster | `@spawn mob <id>` |
| Another map | `@warp <map id>` |
| A skill | `@skill <id> <level>`, then log out and in |
| Levels | `@blvl +N`, `@tmlvl +N` |
| Full HP and MP | `@heal` |

Ids used below: Friend Finder 19512, Mic 19655, TBN Megaphone 19656, Memory Port 19659,
Reidentifier 19647, Befana's Unstable Elixir 19616, Test Dragon Pet 1762, Little Diablo 15800.
Maps: 148 Coral Town, 35 Paradise Monster Guild, 13 Beach Field 3. More in
[GM_COMMANDS.md](GM_COMMANDS.md#ids-useful-for-testing).

## 1. New in v0.1.0

### Login and accounts
- [ ] An account that exists logs in and reaches the island list.
- [ ] An account name that does not exist gets a "LOGIN ERROR" message box.
- [ ] A banned account (operator: `banned = 1`) gets the same box. Lifting the ban lets it in again.
- [ ] Select a character, play, take a few portals: nothing disconnects.
- [ ] Close the client while in the world, start it again at once and log in with the same
  character: you get in, and your progress up to the close is there.

### Operator checks
- [ ] `run-server.bat` starts all four servers; each window shows `ProjectPAULA <ServerName> v0.1.0`.
- [ ] Starting a second copy of a server fails with an error instead of running twice.
- [ ] `stop-server.bat` (or Ctrl+C in the GameServer window) saves the players: after a restart
  their last position and items are there.
- [ ] After a restart with `run-server.bat`, the logs of the previous run are in `logs\`, with
  the date and time in their names.
- [ ] Close the MasterServer window and start `MasterServer.exe` again: within a few seconds
  `login_live.log` shows that the LoginServer connected again, and the character list works.
- [ ] With `log_level: info`, the logs show logins, character enter and leave, map changes and GM
  commands, but no packet dumps.
- [ ] From another network (public server): login, island, character select, the world, a portal,
  and the party window (community link) all work.

## 2. Trade, logout and post box

### Trade between players (A and B)
- [ ] A asks B to trade, B gets the invitation and accepts: the trade window opens on both.
- [ ] Refusing the invitation, or cancelling in the middle: both leave the window.
- [ ] Trade a common item (a whole stack and part of a stack), a rare item and gelder, confirm on
  both sides: the items and the gelder change sides.
- [ ] Chat inside the trade window.
- [ ] With a full bag or too much weight on one side: the trade is refused.
- [ ] Log out and in: the traded items are still there.

### Logout near another player (A and B on the same map)
- [ ] A logs out: B sees the exit effect and A's character disappears.
- [ ] A's pet disappears together with A.

### Post box (A and B)
Open the post box with the `A` key or from the menu. New mail shows up at the next post box check
(`post_sync_minutes`, normally 15 minutes) or when you open a list.
- [ ] A sends a text-only letter to B: it appears in B's post box.
- [ ] B opens it: it becomes "read".
- [ ] A sends a gift (1 item + gelder): A pays the gelder plus the fee (at least 500); when B
  receives it, the item goes to the bag and the gelder is added.
- [ ] A sends a payment request: B pays on receipt; A gets a "payment" letter with the amount
  minus the fee.
- [ ] Deleting an unread received letter returns it to the sender (kept 7 days); deleting a read
  one removes it.
- [ ] The sent list and its sort button.
- [ ] Level gifts: `@levelgift 15`, then open the gift tab and receive the items. Reaching a gift
  level normally sends the gift too.
- [ ] Letters do not show up as "returned" right after login.

## 3. Characters, world, skills, items and social

### Characters
- [ ] Create a character of each race. Check the start in Coral Town, the starter items and the
  starter skill ([CONNECTING.md](CONNECTING.md#5-your-first-steps)).
- [ ] Names that must be refused: `Sigma`, `Gamer`, `Essex`, `Name_1`, a Korean name, a name with 3
  characters. A name that already exists in another case (`kiwi` when `Kiwi` exists).
- [ ] Delete a character: it disappears at once and its name can be used again.
- [ ] Spend free stat points in the status window; log out and in: they stay.

### World
- [ ] Sit and rest: HP and MP recover faster than standing.
- [ ] Shadow monsters (ids 16000 to 16999): only a character with Mind's Eye sees them and is
  attacked by them.
- [ ] "Resurrect" above level 20 with a resurrection item in the bag uses the item; without one it
  is refused.

### Skills and jobs
- [ ] At your class's skill master: change to the 2nd job, then the 3rd job; skill mastery.
- [ ] Skill scrolls: use a scroll item; the skill is cast.
- [ ] GM skills 20000 to 20003 and 8002 ([GM_COMMANDS.md](GM_COMMANDS.md#gm-skills)).

### Items and economy
- [ ] NPC shop: **sell** items (buying already works).
- [ ] Exchange shop: trade items for items.
- [ ] Warehouse: store and take items and gelder; extra tabs with their item; log out and in.
- [ ] Account bank: store an item and gelder with one character, take them with another character
  of the same account.
- [ ] NPC compound.
- [ ] Refine, repair, mature compound and reset.
- [ ] Crazy Drill: `@skill 5019`, then drill until Crazy Drill mode starts (or `@cdm`).

### Social and quests
- [ ] Party: invite, EXP and TM share between members, the item modes, the party board.
- [ ] Monster quests: accept, kill, finish, collect the reward.
- [ ] Scenario quests.

## 4. Regression checks

Run these after an update, or when you have time.

### Basics
- [ ] Island list: one "Ruby Island" button; choosing it (click, `1` or Enter) opens the character
  list. The window title becomes "Trickster Paula".
- [ ] Walking is half the speed of running.
- [ ] Portals and teleports change the map without the login screen.
- [ ] Melee attacks, criticals, evades and blocks; guns with bullets.
- [ ] Drops and pick-up; equip and unequip; potions; throwing items away.
- [ ] Death: "Go to Town" brings you to the last town with 30% HP.
- [ ] Drill: digging finds buried items.

### Whisper (A and B)
- [ ] A whispers to B: B gets the whisper line.
- [ ] Whispering to a name that is not online gives a failure message.
- [ ] The name in another case (`kiwi` for `Kiwi`) works.

### Friends (A and B)
- [ ] A adds B, B accepts: both see each other online. Refusing shows "refused" to A; A can
  cancel a request before the answer.
- [ ] B logs out and in: A sees B leave and come back.
- [ ] Move a friend to another group, rename a group, log out and in.
- [ ] Remove a friend who is online (both lists update) and one who is offline (they see it at
  their next login).
- [ ] A note to an online friend arrives at once; to an offline friend at their next login.
- [ ] Friend position: shows the friend's map; on the same map, the exact position on the
  minimap; while the friend changes maps, "between maps".
- [ ] Go to a friend with the Friend Finder (19512) in the bag: on another map you are moved there
  and the item is used; same map gives "same map"; a friend changing maps gives "moving".

### Polar Bear line
- [ ] A 3rd-job Polar Bear (Zoologist) shows the Zoologist look; the skill window shows the three
  Bear tabs.
- [ ] Hammer Spin (with Rampage Wheel learned) hits all around.
- [ ] Party Guard and Best Shield apply to the party.
- [ ] Crazy Hammer (skill 4801): the gauge appears after login and fills with normal hits (+3 per
  hit) and skills (+1). When full, it uses MP (350 or more) and starts Crazy mode for 20 seconds:
  stronger and faster normal attacks; with the passives II to V also more speed (4804), more AP
  (4806), more critical (4908) and a shock wave around the target (4910). Without enough MP the
  gauge waits full. Dying ends the mode; a map change keeps the gauge and the mode.

### Party skills (A and B in one party, close together)
- [ ] Auras: Pumping Heart 1002, Aura of Mana 2003, Shield All 4004, Aura of Luck 3103, Eagle Eye
  1105, Aura of Haste 1206, Bolster Ballad 2101. A turns one on: B gets the effect. B leaves the
  range: it ends within about 2 seconds; B comes back: it returns. Without a party only A gets it.
  - Bolster Ballad: while A walks only A gets it; standing, the whole party.
  - Shield All with Team Bolster (4103) learned: the DP bonus grows with the members nearby.
- [ ] Bodyguard 4203: B gets the bodyguard effect; a monster hitting B does half damage.
- [ ] Heart's Grace 2216 heals A and B; Med. Cure 2501 removes debuffs from up to 3 members.
- [ ] Party buffs Elemental Boost 2132, Refreshing Dew 2505, Sacred Oath 2504, Leg Training 3208,
  Strong Mind 3303, Gas Mask 3107, Lightning Rod 3108: the icon also appears on B.
- [ ] Heart Spring 5009 and Party Detection Scroll 7046: the members see the effect.

### Megaphones
- [ ] Mic (19655) in the bag, a message in the chat's megaphone mode: B sees the line and a balloon
  over A. The balloon ends when A moves, attacks, picks something up, chats normally, drills or
  shoots. B entering the map later still sees it. The Mic is not used up.
- [ ] TBN Megaphone (19656): use it, write, send. A gets "accepted" with the queue position and
  loses one megaphone; within about 18 seconds everybody sees the banner and the line
  `[world-character] text`. Without a Mic the client says an item is missing. Several in a row go
  out one every 18 seconds.
- [ ] `@sysmega test`: everybody sees `[트릭스터] Paula - test`.
- [ ] Reaching level 400 and taking the 3rd job are announced to everybody (the job shows as
  "JobTab_17" and similar, as in the original).
- [ ] Operator: queue 2 or 3 TBN messages and stop the GameServer before they all went out. After
  the restart the queued messages are not sent and each sender has the megaphone back in the bag.

### Memory Port (19659)
- [ ] The item opens the Memory Port window; save the current place in a slot with a description.
- [ ] On another map, use the item and choose the slot: you are moved there and one Memory Port is
  used.
- [ ] Delete a slot; log out and in: the saved places stay.
- [ ] A map that blocks teleports, and a map that does not allow saving, give their messages.

### Rename and type change
- [ ] Reidentifier (19647): a valid new name succeeds and uses the item; the new name applies after
  a relog (character list, the world, friend lists). An existing or invalid name gives an error
  and keeps the item.
- [ ] Befana's Unstable Elixir (19616), on a new character of one of the 8 basic races before the
  2nd job: the model changes at once to the paired race; the old ears and tail come off and the
  new ones go to the bag. With a full bag it is cancelled and not used up. Polar Bears cannot use it.

### Monster and pet speech
- [ ] Attack Golden Mole 2035, Bluffy Bad Puri 2190 or Grumpy Octopus 2061: sometimes a line when
  the fight starts, and after a critical hit that does not kill.
- [ ] Equip a pet (1762 or 15800): it sometimes talks when it appears, when idle (first after about
  20 seconds, then every 8 to 20 seconds), in battle, when you run, and when you die. Others see the
  balloons too.
- [ ] The Test Dragon Pet 1762 also talks when you sit, rest, level up, TM level up, pick up an
  item or gelder, throw an item away, drop to 30% HP or MP, finish a quest, start drilling, succeed
  or fail a refine, and when a monster quest has 10 seconds or less left.

### Party quest (A as party leader, B in the party)
- [ ] `@warp 35`, talk to Officer Tera, accept the party quest (3005: kill 45 Pineapples per member
  in 20 minutes). Both get the quest window with the timer and the counter.
- [ ] Kill Pineapples (`@spawn mob 2053` or map 13): the counter rises on both screens.
  `@pqkill 85` speeds this up.
- [ ] At the goal: a success message, and a return portal that leads back to map 35.
- [ ] Each player talks to Tera again and gets items, EXP and TM.
- [ ] B (not the leader) cannot accept; a member on another map, or with another quest running,
  blocks the start with a message.
- [ ] Disbanding the party ends the quest with a notice; running out of time ends it silently.
- [ ] Finish, log out before collecting the reward, log in: the reward can still be collected.
- [ ] A map change during the quest keeps the timer and the counter.
