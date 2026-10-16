# Player profiles (runtime/rt_profiles.c)

The plan (user, 2026-10-15): player data moves from the characters to the people playing. A
profile is a person; the multiplayer save keeps lives / coins / power-up per profile ID rather
than per character, so anyone can play any character and keep their own state. Local for now;
a central account server may come later (the ID is meant to be the account key).

## Step 1: profiles (done, 2026-10-15)

A profile: an **ID** (16 hex digits, random, made once, never changes - one per username), a
**username** of 1-4 letters / digits (unique on this PC, shown only in multiplayer, local and
online), a **preferred character** (preselected at the character select as long as nobody else
already has it) and **stats** (global: levels cleared, deaths, coins; per save file: next step).

`<game>/profiles.json`:
```
{ "version": 1,
  "profiles": [ {"id": "D7E052506700B483", "name": "BA", "preferred_character": "luigi",
                 "stats": {"levels_cleared": 0, "deaths": 0, "coins": 0}} ],
  "controllers": { "keyboard": "D7E052506700B483", "remote:001F32ABCDEF": "..." } }
```
`controllers`: the profile each controller (the keyboard; a Wii Remote by its address) was last
used with - the next time it starts on that profile.

**Who's playing?** (rt_menu.c): press 2 > Multiplayer opens it, before Local / Online. One row
per connected controller, each driven by its own controller (rt_remotes.c `remotes_slot_raw`):
Left/Right picks one of the profiles nobody else has taken, *New profile* or *Guest*; 2 takes
it. A new profile asks for the username (Up/Down change the letter, Left/Right move, a blank
ends it; the keyboard can also type it, Enter / Backspace) and the preferred character
(Left/Right), then 2. + on a profile edits it (name, preferred character). When every row is
ready, player 1's 2 goes on to Local / Online; player 1's 1 goes back. Back from Local / Online
returns here.

Into the game: each slot's username and preferred character go to rt_chars.c
(`chars_set_slot_profile`), whose tables are part of the save state - an online game carries
everyone's, the same on every PC. The character select (initOption / readyContents hooks)
starts a player who hasn't picked yet on its profile's preferred character if it's free.
Online, a joining PC's HELLO carries its players' usernames + preferred characters (and its
name is player 1's username); the host puts them into its game for their slots before the game
starts.

Tested (Linux, deterministic, keyboard): creating "BA" with Luigi preferred; on the next start
the keyboard came back on BA and the character select started player 1 on Luigi.

## Next steps

- per save file, per profile ID: lives, coins, power-up (and continues / equipment) instead of
  per character (rt_mod.c save conversion, the module's per-type arrays); per-save stats;
- counting the stats (deaths: daPyMng_c::startMissBGM; level clears: setDemoGoal; coins:
  dMultiMng_c::incCoin - address to find), for the PC's own players only;
- the username over each player in the game (multiplayer only);
- deleting profiles; a profile chosen for a slot shown on the character select card;
- maybe a central account server (sign-in, the same profile on several PCs).
