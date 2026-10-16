# TODO

Shared list for the user and both Claude instances. Keep it current: when you start something,
put your name on it; when it's done, move it to "Done" with the date (details go in
`CHANGELOG.md`). The user decides priorities - ask before reordering "Next up".

Legend: `[ ]` open, `[~]` in progress (who), `[x]` done.

## Next up (user's list)

- [~] **Native port: from emulation to a decompiled PC port** (user, 2026-10-16: performance,
  modding and principle; online play may be taken apart) - `docs/NATIVE_ROADMAP.md`.
  Split (user, 2026-10-16): track A (platform: SDK libraries native, no emulated hardware)
  instance 1; track B (game code decompiled class by class) instance 2.
  - [~] A1 GX (FIFO/CP/PE/PI native: done; GX state functions + the pipe: next)  - [ ] A2 OS threads/timing  - [~] A3 disc/saves (DVD reads + IOS client native: done; DI boot calls, NAND straight to host files: next)
  - [ ] A4 input  - [ ] A5 audio  - [ ] A6 hardware layer deleted
  - [x] online play may be paused while track A runs (user, 2026-10-16)
- [~] **Player profiles** (user, 2026-10-15) - `docs/PROFILES.md`. Player data moves from the
  characters to the people playing.
  - [x] step 1: profiles (ID, 4-letter username, preferred character, stats fields),
    Who's playing? per controller, preferred character preselected, online HELLO carries them
  - [ ] multiplayer save: lives / coins / power-up per profile ID instead of per character;
    per-save stats
  - [ ] count the stats (levels cleared, deaths, coins) for each PC's own players
  - [ ] usernames over the players in the game (multiplayer only)
  - [ ] delete a profile; show the profile on the character select card
  - [ ] maybe: a central account server (same profile on several PCs)
- [~] **Update check / auto-update** (user, 2026-10-15) - `docs/UPDATES.md`. Done: the update screen
  after "hold the Wii Remote sideways" (spinner, download bar, skip), a signed update list
  (Ed25519) + files by SHA-256 on the user's copyparty server, `tools/publish_update.py`
  (keygen / publish / upload), install by renaming the running exe + pending files at the next
  start, version in Options, online HELLO refuses a different version.
  - [ ] **first run on Windows** (WinHTTP, renaming the running exe, the restart)
  - [ ] the user's server address (`update_url.txt`) and key (`keygen`)
  - [ ] maybe: compress the files on the server (the exe is ~62 MB; it shrinks a lot)
  - [ ] maybe: show the update's notes on the screen; a "check for updates" in Options
- [ ] **4:3 overhaul** (user, 2026-10-15): 4:3 must show *more playfield vertically* instead of
  zooming in (the current implementation is wrong). Note for online play: the aspect mode
  changes the camera, so it changes what the game simulates (spawns, culling) - it must be the
  host's in a session (a host-decided setting).
- [~] **Online multiplayer** (Claude instance 2) - `docs/ONLINE.md`. Direct IP, whole session.
  - [x] phase 1: connection, host/join screen, online players in the host's slots
  - [x] model: deterministic lockstep, then rollback (user, 2026-10-15)
  - [x] deterministic mode: virtual clock, inputs latched per video field, state hash
    (`rt_det.c`); two runs with the same inputs identical (incl. random host delays)
  - [x] online session: host starts it, save + settings + player map sent, everyone restarts
    into lockstep (input delay), state-hash checks (loopback: identical over 2600 fields)
  - [ ] test on two real PCs (Windows, GL) over the internet
  - [ ] two copies on one PC share the real Wii Remotes (both read them): fine with the keyboard
    in one window; for one-PC tests a remote should belong to one copy
  - [ ] a client goes back to its own game and leaves the lobby when the game ends: maybe stay
    in the lobby
  - [ ] while waiting for the others, draw a message in the window (now only the title bar)
  - [ ] a client can't use our menus in the session (only player 1 drives them): a local
    "leave" for clients, and keep a PC that leaves from ending the game for everyone (its
    players could stand still)
  - [~] check the game version and `mod/` folder match before starting a session (the version: done, HELLO)
  - [x] save states (rt_state.c, docs/SAVE_STATES.md): in-process and across copies, verified
  - [x] the online game starts from the host's running game (no restart), back to your own game
    at the end (user, 2026-10-15)
  - [ ] Windows: first run of the new fibers / fixed addresses / save states (untested there)
  - [x] speed at high ping (user, 2026-10-16: 20 FPS at 150 ms): direct path (a client runs a
    field once it has everyone's buttons, not once the host published it), automatic input
    delay from the pings, automatic RESYNC instead of OUT OF SYNC
  - [x] rollback (user, 2026-10-16: "the goal was 0 ms input latency"; players who couldn't
    catch up watched the game like a video): the default now, input delay 0. States of the last
    48 fields from written pages only (Windows GetWriteWatch / Linux mprotect), undo lists,
    silent re-runs, the host forwards buttons and decides for a PC that's away, time sync,
    catching up without states, the host waits for everyone to load. Identical in every
    Linux test (150/300 ms, 3 PCs, drops, rejoin) - docs/ONLINE.md "Rollback"
  - [ ] rollback on Windows: GetWriteWatch, the speed of re-running fields with a real
    renderer, a game over the internet
  - [ ] rollback: a direct client-to-client path for buttons (now through the host: two trips)
  - [ ] a rollback can make another player jump: smoothing / showing them a little later is
    possible but not done
  - [x] user: smoother online flow: Multiplayer > Online, host chooses game + file, starts at
    the character select (2026-10-15)
  - [x] user: a player dropping out / closing doesn't end the game; reconnect, rejoin
  - [ ] a dropped player's characters just stand there: show them as disconnected in the game
  - [ ] two players rejoining at the same moment (the second one's state replaces the first's
    while it loads: it fails its page check and has to join again)
  - [~] user: settings follow the host, except local ones (volume, controllers, display) and
    character picking - done inside a session (the host's settings.ini, local keys kept); the
    lobby screens themselves still need it
  - [ ] user: several local players from one PC (the protocol has up to 4 per PC; needs UI and
    testing)
  - note: the null renderer and the GL renderer drift apart (from field ~2394 in 1-1: g3d
    matrices); GL vs GL is identical - all peers use GL. Find what the guest reads that depends
    on the renderer.
- [~] **Wii Remote handling** (Claude instance 2) - `docs/WIIMOTES.md`. Up to 8 real remotes,
  pairing for good from Options > Controls (SYNC button), a connecting remote takes the next
  free player slot (Controls can move things for the session), player lights.
  - [x] code: Windows pairing + 8 HID remotes, slots/memory/lights, 4 WPAD remotes, GC ports 5-8
  - [x] pairing hung (BluetoothAuthenticateDeviceEx + callback never returned): Dolphin's way now
  - [x] slots: next free slot on connect (2026-10-15; fixed slots before); Left/Right in
    Controls changes a slot's controller for the session; keyboard in any slot (kept)
  - [x] remotes switch off when the game closes
  - [ ] user: test pairing (after "Remove device" in Windows too), reconnecting, slot changes
    with real remotes on Windows (log lines `[wm]`, `[remotes]`)
- [~] **Character list: 12 characters, room for more** (Claude instance 2) - `docs/CHARACTERS.md`.
  Roster (in order; first five on the title screen): Mario, Luigi, Ala Gold, Buckenberry,
  Toadette | Toadsworth, Daisy, Rosalina, Goomba, Retro Mario (SMB1), Retro Mario (SMW), Retro
  Mario (SM64). Purple Toadette is dropped.
  - [x] character table (`runtime/rt_chars.c`); players remember the character they picked
  - [x] character select: NSMBW's cards walk all 12 (left/right); portraits from files
  - [x] title screen: first five only
  - [x] portraits: from the mods (Daisy, Rosalina, Goomba), HUD icons scaled (Toadette,
    Toadsworth), placeholders (retro characters) - [ ] user: better ones where wanted
  - [x] models from the mods: Toadsworth, Daisy, Rosalina (`runtime/rt_charmdl.c`)
  - [x] singleplayer: lives / power-up follow the save file whatever the character
  - [ ] world-map figures, voices, HUD icons for Toadsworth / Daisy / Rosalina
  - [ ] per-character lives / power-ups (today per body: Retro Mario shares Mario's)
  - [ ] more than 8 player types, so no two characters share a body (today Mario and Retro Mario
    can't play together if Retro took Mario's body first)
  - [ ] save format v2 (per character; remember each slot's character)
  - [ ] Goomba's model (from its mod) - needs a 9th body
- [~] **Retro Mario (SMB1)** (Claude instance 2) - `docs/RETRO_MARIO.md`.
  - [x] SMB1 physics ported from the disassembly (`runtime/smb1_physics.c`, tests in
    `tools/test_smb1_physics.c`)
  - [x] sprite sheet templates (`ui/retro/mario/`); [x] user: small sheet painted;
    [ ] user: super, fire, ice, propeller, penguin, mini
  - [x] an NSMBW player driven by the SMB1 physics, collision fed back (`runtime/rt_retro.c`)
  - [x] drawn as a 1-tile sprite plane, SMB1 animation rules
  - [x] no 3D model anywhere: world map, select figure, ... are the sprite plane too
    (left/right only); [ ] HUD head icons (2D) for the retro characters
  - [x] selection: an entry in the character select (character 10)
  - [ ] more pictures in NSMBW states (carry, throw, grow, shoot, pipes, victory), play-testing
- [ ] **Retro Mario (SMW), Retro Mario (SM64)** - characters 11 and 12 of the roster: same
  approach as SMB1 (a movement port + sprites), designs to settle with the user.
- [~] **Discord Rich Presence** (Claude instance 1, 2026-10-15) - `docs/DISCORD.md`. What you're
  doing (title, world map, level, online) on your Discord profile, through Discord's local IPC
  (no SDK). Code done (2026-10-16).
  - [x] Discord application ID in `recomp/discord_app_id.txt` (2026-10-16)
  - [ ] user: art assets in the Discord application (`logo`, character ids) - docs/DISCORD.md
  - [ ] first run with the real Discord app (Windows)
- [ ] **Audio controls in the config** - music and sound-effect volume (master too?) in Options,
  saved to settings.ini, applied live.
- [ ] **Pause menu with settings** - the in-game pause menu gets access to some Options entries
  (which ones: to decide - e.g. audio, display, controls).

## Open (from earlier work)

- [ ] **Multiple controllers / players joining** - the multiplayer flow is built (8-player
  character select, Start Game once players 1 and 2 have chosen), but only one controller is
  read, so a second player can't join yet. Goes with "Wii Remote handling" (and keyboard /
  other pads per player).
- [ ] **Controls screen** - still a placeholder (keyboard list). Should become per-player
  control settings (rebinding, which device is which player).
- [x] **Boot: "Do not hold your keyboard sideways."** - the controller screen for a keyboard
  player 1, after the console's "Hold the Wii Remote sideways." (user's drawing, 3D keycaps,
  game font) - docs/BOOT_SCREEN.md (2026-10-16)
- [x] **Update screen in the boot's real panel** (rounded, game font for its text) (2026-10-16)
  - [ ] not run on Windows yet (the layout positions in widescreen modes, the fade)
- [ ] **Game Select** - pinned: currently one card. Meant for level and map packs;
  needs restructuring (where packs live, how a pack replaces stages/maps) before more entries.
  - [x] game portraits: `ui/graphics/gameportrait/<id>.png` on the card, which takes the
    picture's shape (user, 2026-10-16) - docs/GAME_SELECT.md
  - [ ] with several games: the cards side by side (the file select has three places), the
    game's name under / over its portrait
- [ ] **Free-for-All / Coin Battle** - reachable from the multiplayer File Select and go to the
  8-player character select; nothing past that has been tested.
- [ ] **Native port of the 8-player module** - 13 of 1318 functions are native (`NATIVE_PORT.md`,
  `python3 tools/native_port_status.py`). Next: the rest of the character select
  (`dNumberOfPeopleChange_c`, `dCharacterChangeSelectBase_c`), then the save/NAND code.
- [ ] **Module save names** - renamed at link time (`tools/link_mod.py`); goes away when the
  save code (`dNandThread_c`) is ported.

## Known issues

- Input is ignored during the ~0.5 s fades between setup screens (on purpose - a press during a
  fade used to freeze the menus).

## Done

- [x] 2026-10-15 - SMB1 Mario: no slope slide / small crouch / carrying / Yoshi (and the Yoshi
  model bug), SMB1 goal pole (slide, swing, walk into the castle), vines left/right only, clean
  swim frames, no particles (level, water, map), big Mario sheet, SMB1 sound effects.
- [x] 2026-10-15 - Wii Remotes take the next free slot when they connect (positions not kept).
- [x] 2026-10-14 - Wii Remotes: fixed player slots set in Controls (Left/Right: keyboard /
  remote / none, swapping), keyboard in any slot; pairing rewritten the way Dolphin does it.
- [x] 2026-10-14 - Wii Remotes: pairing dialog (SYNC, permanent), up to 8 real remotes
  (players 1-4 Wii Remote channels, 5-8 GameCube ports), remembered slots, player lights.

- [x] 2026-10-14 - Singleplayer character select: Start Game only once chosen, centred, no Back
  button; 1 takes the character back, 1 again goes back to the File Select.

- [x] 2026-10-14 - Retro Mario: the sprite plane replaces the 3D model everywhere (world map,
  character select figure, ...), left/right facing rules from the user.

- [x] 2026-10-14 - Select cards: the original slide is back (both faces show their character);
  the card window clips them again (a 4:3 scissor hack had removed the game's clip).

- [x] 2026-10-14 - Select: one portrait at a time; Toadsworth/Daisy/Rosalina models and mod portraits;
  Retro Mario smooth, follows slopes, no particles; singleplayer state follows the save file.
- [x] 2026-10-14 - Character select walks the 12-character roster; portraits for every character
  (the missing ones for characters 5-8 were a bug); title screen shows the first five.
- [x] 2026-10-14 - Retro Mario (SMB1) playable: SMB1 physics + sprite, picked in the character select.
- [x] 2026-10-13 - Title screen: buttons other than 2/A started singleplayer directly.

- [x] 2026-10-13 - Multiplayer: pressing 1 (or 2) while the character select was fading in
  froze input (fixed: fades restart if refused; input held during fades).
- [x] 2026-10-13 - The 8-player module's author name removed from the source (third-party notice kept in
  `THIRD_PARTY_NOTICES.md` as the MPL requires).
- [x] 2026-10-13 - Esc no longer closes the game.
- [x] 2026-10-13 - Title screen: players use the dash animation while running off/back in;
  everyone is in Super Mushroom form like the original.
- [x] 2026-10-13 - One-step Windows build (`build_windows.bat` / `build.py`).
- [x] 2026-10-13 - Setup menus: Game Select -> File Select -> character select, fades,
  "Select Players" removed, 8-player multiplayer flow, "Settings +".
- [x] 2026-10-13 - 8-player module built in (settings keys and save file renamed), first
  native subsystems (game setup scene, title screen).
- [x] 2026-10-13 - Random freeze (lost wake-up), GPU selection, hitching; 10 s singleplayer wait.
