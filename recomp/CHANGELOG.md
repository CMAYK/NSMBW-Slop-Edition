# NSMBW PC port - changelog / hand-off notes

Notes between the Claude instances working on this project. Newest first. Each entry says what
changed, why, how it was verified, and what is still open, so the next session can pick up
without re-deriving it.

---

## 2026-10-16 (22) - Claude (instance 1): track A3 - the disc and the IOS client without IPC hardware

- **Disc reads are native** (`runtime/rt_dvd.c`): DVDReadAbsAsyncPrio (fn_801CECC0), DVDReadPrio
  and DVDCancel copy straight from the disc image; the command block's callback runs "from an
  interrupt" through a new native event queue (`rt_post_callback` in `rt_core.c`: guest callbacks
  delivered as an external-interrupt-like frame, no hardware register involved).
- **The IOS client is native** (`runtime/rt_ios.c`): IOS_Open/Close/Read/Write/Seek/Ioctl/Ioctlv
  and their Async forms go straight to the device code (DI, FS/NAND, ES, STM, Bluetooth); async
  callbacks through the same event queue. Request blocks live at guest 0x82010000 (128 x 0x40).
  The IPC registers and IPC interrupt are no longer used after IPCCltInit (checked with
  RT_MMIO_INVENTORY); no DI registers either.
- Save states / rollback now cover 0x80000000-0x82100000 (33 MB: + the native GX register
  shadow and IOS request blocks), so old state files don't load (new build anyway).
- Tested headless (deterministic GL run: boot, title, file select, new game cutscene; saves);
  Windows build OK. Not tested: real Wii Remotes over Bluetooth on Windows - please check
  pairing/reconnect still work.

## 2026-10-16 (21) - Claude (instance 1): native port track A started - GX without emulated GX hardware

User: go ahead with docs/NATIVE_ROADMAP.md (instance 1: track A, the platform; instance 2: track
B; online play may be paused).

- **Inventory tool** - `tools/hw_inventory.py` (static: functions that load hardware addresses)
  and `RT_MMIO_INVENTORY=<file>` (runtime, rt_hw.c: every distinct guest function + hardware
  register it touched, written when the game exits). Baseline: GX touched CP/PE/PI/MI;
  OS, VI, DSP/AI, EXI, SI, DI and IPC still do.
- **GX FIFO native** (rt_gx.c "the GX FIFO, native"): `__GXFifoInit` points the GX library's
  register pointers (__cpReg/__peReg/__piReg/__memReg, r13-0x4DEC..) at plain memory
  (0x82000000+), so the library never reaches the emulated CP/PE/PI registers again. Commands
  from the write-gather pipe: linked CPU/GP FIFO -> executed directly (no copy into the guest's
  FIFO ring any more); not linked (display list recording) -> written into the CPU FIFO's buffer.
  Native/wrapped: GXSetCPUFifo, GXSetGPFifo, fn_801C32E0 (__GXSaveFifo: pointers from our
  writer), GXGetGPStatus (idle), fn_801C4B70 (GXAbortFrame), the PE token/finish interrupt
  handlers (acknowledge here). `hw_pi_fifo_*` removed; CP/PE register accesses log
  "unexpected ... (track A1)".
- Verified (Linux, GL): deterministic runs before/after, title + menus: same pictures (the
  frames between differ by a few frames of timing - the virtual clock counted the removed
  register reads); `RT_MMIO_INVENTORY`: no CP/PE/PI/MI access by GX code any more. Windows build
  clean (game\nsmbw.exe updated).
- Still emulated for GX: the write-gather pipe itself (commands are still GX command bytes; the
  GX state functions and nw4r's inline vertex writes produce them) and the token/finish
  interrupts (they go when the OS layer does, A2).

## 2026-10-16 (20) - Claude (instance 2): the boot's controller screen for keyboard players; the update screen in the real panel

User: a picture of hands holding a keyboard sideways under a red crossed circle, "do not hold your
keyboard sideways", in the style of the "Hold the Wii Remote sideways" screen. The user supplied
the drawing, then asked for:
- consistent keys;
- the console picture's skin colour and line thickness;
- hands as big as the console's, then 15% smaller twice;
- a 20% larger sign with a thinner line, allowed outside the picture box;
- then "implement it into the game, use the correct font and all".

- `runtime/rt_bootscreen.c` (new; added to `build.py` RUNTIME), docs/BOOT_SCREEN.md:
  - When player 1 is on the keyboard, the boot's controller-information layout (`N_contInfo_00`)
    shows `ui/graphics/boot/keyboard_sideways.png` instead of the remote picture. It goes into
    `P_yoko_00`'s texture as RGB5A3, and the pane is resized to the box's inside width (292
    units) divided by its 0.7 scale.
  - Its text boxes (`T_contInfo_00` / `_01`) say "Do not hold your\nkeyboard sideways." in the game's
    font.
  - The picture is 200 units tall against the box's 172, and the pane draws after the box, so the
    red sign shows over the box's frame.
  - Hooks: `boot_layout_calc` / `boot_layout_draw`, from `rt_menu.c`'s layout hooks.
  - Testing: `RT_BOOT_KEYBOARD=0/1`, `RT_BOOT_DEBUG=1`.
- `rt_gamesetup.c`: `gs_ui_mem_borrow()` lends the game-picture region (0x81870000, 576 KB) to
  the boot screen (456 KB). The first game portrait takes it back (`boot_screen_mem_lost`).
- `tools/make_boot_picture.py` (+ `tools/art/keyboard_sideways_source.png`, the user's drawing)
  makes the PNG reproducibly: the keys redrawn, the console's colours, lines grown, the window,
  the sign. Options for size, sign, sign line and line weight. Its output matches the shipped PNG.
- Then the user asked for mechanical (3D) keycaps: `--keys 3d` (default) gives darker sides and a
  lighter top set in, more on the side facing the viewer. The PNG was made again. It stays a
  plain file the game reads at start, so it can still be edited or replaced (docs/BOOT_SCREEN.md).
- **The update screen uses the real panel.** User: "round the edges of the updating screen? It
  should be using the same UI panel element yet it seems to use some sort of recreation".
  - It is now the boot's controller layout itself, which is still on screen while the boot waits
    for the update: the panel `W_contInfo_00`, rounded.
  - Over 0.3 s the picture box, the "2" button and the old text fade out, and the update's text
    fades in, in the game's font. It goes on one or two lines (broken near the middle), and the
    text moves up to y -40.
  - `rt_menu.c` `update_screen` no longer draws a sky or a panel. It draws only the spinner /
    download bar, the speed, the version (where "2" was), "1: Skip" and the failure reason, placed
    by the layout-to-overlay mapping.
  - `boot_update_frame` (from `hle_DoSceneChange`) fades the fader back in should it be black.
  - Testing: `RT_UPDATE_FADE=s`, `RT_BOOT_TRACE=1`.
- **The order** (user: "so hold wii remote, then dont hold keyboard"): the console's screen
  comes first, unchanged. 2 then gives the keyboard screen (a 0.3 s cross-fade of picture and
  text), and 2 again goes on to the update screen or the title.
  - `boot_kb_hold()` holds the boot's title request, ahead of the update hold.
  - `boot_kb_press()` is called from `rt_menu.c`'s input filter.
  - Skipped in deterministic test runs unless `RT_BOOT_KEYBOARD=1`.
- Found while doing it: `P_yoko_00` has a 0.7 scale. The first try came out at 70%.
- Merged with Claude instance 1's Discord work on the PC (rt_discord.c, rt_menu.c's
  `discord_frame`, build.py, build_linux_clang.sh, rt_core.c, version_header.py), which had changed
  there meanwhile. rt_bootscreen.c was added to both build scripts.

Verified (Linux, GL/llvmpipe):
- The console's screen, 2, the keyboard screen, 2, then the title. With an update server set,
  the update screen comes after the keyboard screen. The keyboard screen is in the game's font.
  With `RT_BOOT_KEYBOARD=0` the boot is as before.
- The update screen: checking with the spinner, then "Couldn't reach the update server" (server
  down). A local copyparty with 0.1.1 showed the download bar, speed, skip and version. The
  change between the two screens was checked frame by frame at 3 s.

## 2026-10-16 (19) - Claude (instance 1): Discord Rich Presence

User: implement Discord rich presence. Details: **docs/DISCORD.md**.

- `runtime/rt_discord.c` (new): Discord's local IPC spoken directly (no SDK): Windows named pipe
  `\\.\pipe\discord-ipc-N`, Linux socket (`$XDG_RUNTIME_DIR`, Flatpak, Snap paths). A background
  thread connects (retry every 15 s while Discord isn't running), HANDSHAKE, then SET_ACTIVITY at
  most every 4 s with the newest text; reconnects if Discord restarts. Not in save states.
- What it shows (`discord_frame()`, called at the top of `menu_draw`, looks twice a second):
  title / "Getting ready" (setup scene) / opening / "World N map" (`dScWMap_c::m_WorldNo`
  0x8042A24D) / the level (`dScStage_c::m_instance` 0x8042A1C8, +0x120C world, +0x120D stage:
  "World 1-2", "World 3 - Tower", "Airship", ...); second line Singleplayer / Multiplayer /
  Online, N players; party size from `mPlayerEntry`; time since start; large image `logo`,
  small image = player 1's character id.
- Application ID at build time: `discord_app_id.txt` -> `tools/version_header.py` ->
  `GAME_DISCORD_ID` in out/rt_version.h. **Not set yet** - without it presence is off and the log
  says so. settings.ini `discord_presence` (default 1, local: not taken from an online host).
- build.py / build_linux_clang.sh: + rt_discord. `tools/test_discord_ipc.py`: a fake Discord.

Verified (Linux, rt_discord.c with a stub harness + the fake Discord): handshake with the ID,
title -> World 3 map (1 player) -> World 3 - Tower (3 players, party 3/8) -> 3-2 -> 3-3 all
arrived in order. Windows build clean (exe deployed); not run against a real Discord yet.

## 2026-10-16 (18) - Claude (instance 2): game portraits on Game Select

User: implement `ui/graphics/gameportrait` - "this is where portraits for game selections are
stored" (it holds `nsmbw.png`, a 1000x1407 cover).

`rt_gamesetup.c` (docs/GAME_SELECT.md):
- **The card shows the picture.** Game Select's card shows `ui/graphics/gameportrait/<id>.png`
  (ids in `GAMES[]`, only `nsmbw` for now).
  - The picture is scaled to fit 512 and compressed to GX CMPR by our own DXT1 encoder:
    360x512, 90 KB.
  - It is kept in guest memory at 0x81870000-0x81900000, a free gap after the module's loader
    scratch.
  - The card's gradient pane `P_NumBase_01` gets it as its texture, with white vertex colours.
- **The card takes the picture's shape.** Its panes are narrowed before the matrices are
  computed, so the white border stays 5 units wide. The name ("NSMBW") shows only without a
  picture.
- **The select cursor follows.** `dSelectCursor_c` copies the card's size when it is set on it,
  so the copied width is corrected (both ways).
- **Everything is put back** for File Select. Without a picture the card is the old one.

Verified (Linux, GL/llvmpipe screenshots): title > Singleplayer > Game Select showed the cover
on a narrowed card with the corners around it. 2 went to File Select with three normal cards
and normal corners. 1 came back to Game Select with the cover and fitted corners again. The
fades went through cleanly.

## 2026-10-16 (17) - Claude (instance 2): rollback netcode (0 ms input delay), catching up fixed

User: "the goal was 0ms input latency, 250ms is unplayable", and some players couldn't catch up
with the host. They played the game like a video until they were dropped.

Causes in the lockstep game:
- The automatic delay took the worst of 6 pings, so one 568 ms spike made it 15 fields (250 ms).
- A client that loaded the host's game (9 MB) while the host played on caught up while drawing
  every frame, which was too slow. Then away/back cycles, a resync, and finally the drop.

Rollback (docs/ONLINE.md "Rollback"; lockstep stays as the fallback, `net_rollback = 0`):
- `rt_state.c` `rb_*`: the state of each of the last 48 fields, made of written pages only.
  - MEM1/MEM2/ARAM are watched: Windows `GetWriteWatch` (`MEM_WRITE_WATCH` in `map_fixed`),
    Linux mprotect + SIGSEGV. The locked cache and the runtime's state sections are compared.
    The stacks are copied.
  - Undo lists; going back applies them newest first, then the host fiber jumps into `rb_save`
    of that field.
  - Fields whose buttons are final keep no state (`rb_save(F, keep)`), which makes catching
    up cheap.
  - File reads into guest memory go through `rt_fread_guest`.
- `g_resim`: fields run again silently (no draws/EFB copies/audio/sfx, unpaced).
- `rt_net.c` `rb_field`, plus SPEER/SINPUT/SFRAME handling:
  - Prediction is the last known buttons; a wrong guess means a rollback.
  - The host forwards buttons at once and publishes final fields. It decides for a PC that's
    away (1.5 s), and that PC is back when its buttons are current again.
  - SINPUT resends everything not acked, in up to 4 parts. A waiting client asks for its gap
    (SFREQ).
  - Window: 20 fields. Time sync uses `det_pace_slow`, which moves the pacing's clock.
  - Catching up is silent and unpaced and logs its speed. The host waits for everyone to load.
- Chunk window 96 -> 320: joining again went 0.7 -> 2.5 MB/s at a 150 ms round trip.
- Test hooks (`RT_NET_BLIP_AT` etc.) work in both kinds of game. New: `RT_NET_LOG_USED` (the
  buttons each field ran with).

Bugs found on the way:
- Desync: buttons arriving after a field had decided its own, from `net_update`'s poll in the
  middle of the field, weren't counted as a wrong guess (`f < now` missed `f == now`).
  Found by comparing `RT_NET_LOG_USED` files; fixed with `SS.decided`.
- An away PC never came back: the host kept deciding "no buttons" up to its own field, so the
  PC's buttons were always for fields already decided.
- A 3 s drop both ways: the client waited forever on its gap (the host only resends the last
  16 fields). It now asks for the published fields.
- Time sync never slowed anything (a plain sleep that the pacing made up for).

Verified (Linux, null renderer, both pressing buttons):
- 150 ms and 300 ms round trips: identical, no waiting.
- 3 PCs: identical.
- A client's packets lost for 0.5 s and 3 s, and the network down for 3 s: identical, back.
- Quit + rejoin: caught up at 194 fields/s; identical.
- Lockstep (`net_rollback = 0`) still identical.
- Local `RT_RB_TEST`: 0 differences.

Open: never run on Windows (GetWriteWatch, a real renderer re-running fields). Buttons between
clients go through the host.

## 2026-10-16 (16) - Claude (instance 2): build_windows.bat asks for the version number

User: set the version from build_windows.bat instead of editing VERSION.

- `build.py --ask-version` (build_windows.bat passes it): before building, shows this build's
  version and, when update_url.txt's server answers, the version players have; Enter keeps it,
  `+` takes the next (last number + 1), or type one (checked: digits and dots); written to
  VERSION. `build.py --version X` sets it without asking. The publish question after the build
  is unchanged (asked when the version is higher than the server's).
- Verified with a pty: invalid input asked again, `+` 0.1.0 -> 0.1.1, typed 0.3.7, Enter kept;
  "(players have 0.2.0)" read from a local copyparty.

## 2026-10-16 (15) - Claude (instance 2): online play at high ping: 60 FPS (direct path, automatic delay)

User: 2 players, 150 ms ping: the game ran at ~20 FPS.

Cause: a client ran a field only once the host had published it (after collecting everyone's
buttons), so its own buttons went to the host and back first. The delay was 3 fields (50 ms)
against a 150 ms round trip: 3 fields per round trip = 20 FPS. Reproduced with a new lag
simulator: 22.9 FPS.

- `rt_net.c`:
  - **Direct path**: SPEER (h->c) carries the host's own buttons every field, each client's
    buttons forwarded to the others the moment the host accepts them, and the field from which
    the host uses the receiver's buttons. A client runs field F as soon as it has everyone's
    buttons (`direct_field`), else the published field. Fields run directly are checked against
    SFRAME; a difference (the host went on without us) -> RESYNC.
  - **Automatic input delay** (`auto_delay`): the worst of each client's last 6 pings; half the
    worst trip (client <-> client counts both halves) + 8 ms, in fields, + 1; 2-15.
    settings.ini `net_input_delay = 0` = automatic (new default; the old default line `= 3` with
    its old help text is rewritten by `settings_migrate_file`). The window title shows the delay.
  - **RESYNC** (c->h) / epoch: the client freezes, the host takes a state at the next field and
    sends the differing pages (the rejoin machinery without leaving); SINPUT carries an epoch so
    buttons from before don't count. A failed state check now does this instead of OUT OF SYNC
    for good (3 in a minute: OUT OF SYNC).
  - Catching up: asks for the next fields ahead of time; fields more than 496 ahead aren't stored
    (they overwrote fields still to come after a long absence - a rejoin at 150 ms never caught up).
  - Host: session PONGs keep the pings fresh. Test hooks: `RT_NET_LAG=ms[:jitter]`,
    `RT_NET_NO_DIRECT=1`, `RT_NET_BLIP_OUT_AT=field:ms`; the held packets drain on close.
- Verified (Linux, two copies, RT_NET_LAG=75 each):
  - before: 22.9 FPS; now: delay 6 (100 ms), 59.9 FPS, ~0 waiting, all fields direct, hashes
    identical (42 checks);
  - jitter 60±30: delay 7, 59.9 FPS, identical;
  - 3 PCs: identical (there both paths arrive about together);
  - forced desync: recovered (0.1 MB), identical afterwards;
  - one-way loss 2.5 s: "field 2013 differs", recovered, identical;
  - quit + rejoin: identical, caught up, direct path.
- Not done: rollback (TODO.md, with measurements).

## 2026-10-15 (14) - Claude (instance 2): copyparty login check says what's wrong

User: `login` says "that login can't write and delete in <folder> (it has: nothing)".
"nothing" only meant copyparty's `?ls` didn't answer 200 + JSON there - no detail.

- `publish_update.py`: `access()` replaces `perms()`: reads copyparty's `?ls` for the folder and
  reports who copyparty thinks you are (`acct`: "*" = the login wasn't accepted) and what you may
  do there; a folder that answers 404 (doesn't exist yet, or hidden from this login) falls back
  to its nearest parent; nothing answering like copyparty lists each address + HTTP status + the
  start of the reply. Requests send `User-Agent: nsmbw-publish/1` (proxies such as Cloudflare
  block "Python-urllib").
- Verified against a local copyparty (volume /cmayk rwmd you, /cmayk/nsmbw r:* + rwmd you, a
  second account rw only): right login -> ok; wrong password -> "doesn't accept that login";
  a missing subfolder -> parent's rights; rw-only account -> "you may only: read, write, get";
  no server -> "can't reach"; publish as the rw-only account stops before uploading.

## 2026-10-15 (13) - Claude (instance 2): build_windows.bat offers to publish; your copyparty login

User: roll publishing into build_windows.bat; log in to copyparty as the user; nobody else
should have delete.

- `build_windows.bat` runs `build.py --ask-publish`: after the build, `publish_update.py publish
  --ask` compares VERSION with the server's latest.txt; same or older: one line, nothing else;
  higher: "Publish version X as an update for players? [y/N]", then notes, pack, upload.
- Login: user name + password, sent as HTTP basic auth (copyparty accepts `user:password`,
  with or without `--usernames` - both tested); checked first with copyparty's `?ls` perms
  (must have write + delete there); saved after asking in `~/.nsmbw_upload_login` (Windows:
  DPAPI-encrypted for the user's account; untested on Windows), forgotten when it stops
  working; `publish_update.py login` / `logout`. The final check reads latest.txt with no login
  (as players do).
- Delete: only the user's account in the update folder (config in docs/UPDATES.md); an
  anonymous PUT with Replace is refused (401, tested).
- Verified (Linux, local copyparty, a pty driving the prompts): same version -> skipped; higher
  -> asked; wrong password -> asked again; right -> saved; second publish used the saved login.

## 2026-10-15 (12) - Claude (instance 2): publishing from the build; copyparty setup

User: how to make only the nsmbw folder readable on copyparty (GUI?); building doesn't publish.

- copyparty has no permissions GUI: docs/UPDATES.md has a config file with a private root and
  an `/nsmbw` volume readable by everyone (`r: *`, `rwmd: <you>`).
- `build.py --publish [--notes ...]`: after deploying, publishes the game folder. The build
  prints `version X - updates from <address>` or `updates OFF in this build (why)`.
- `publish_update.py publish` uploads to the folder in `update_url.txt` by default (`--upload`
  another, `--no-upload` none), asks for the copyparty password and the notes when not given.
- `publish_windows.bat`: double-click to publish (`publish_windows.bat keygen` for the key).
- Verified: `build.py --publish` against a local copyparty: built, deployed, packed 41 files
  (92.8 MB -> 34.4 MB zip), uploaded, latest.txt read back.

## 2026-10-15 (11) - Claude (instance 2): crash at start fixed; the update is a zipped build

User: the game crashes instantly; and a script that makes a zipped build to host for the
updater.

- Crash: `RT_VERSION` (the version macro from out/rt_version.h) is also a windows.h macro
  (`MAKEINTRESOURCE(16)`), defined after ours in rt_update.c, so on Windows the version "string"
  was the pointer 16: the first log line (`[update] version `) crashed reading address 0x10
  (seen in the user's nsmbw_log.txt; the disassembly passed `$0x10`). Renamed to
  `GAME_VERSION` / `GAME_UPDATE_URL` / `GAME_UPDATE_KEY`; rt_update.c copies them into char
  arrays (a non-string would no longer compile). Disassembly now passes the string's address.
  (The same pointer reached `rt_version()`: the Options version line and online HELLO.)
- `tools/publish_update.py publish` now makes `nsmbw-<version>.zip` (deflate 9) + `latest.txt`
  format 2: per file its SHA-256, size, offset of its data in the zip, packed size, method. The
  game (`rt_update.c`) fetches each changed file as an HTTP range of the zip, inflates it
  (stb_image's zlib decoder), checks size + SHA-256. Only `nsmbw-update 2` lists are accepted.
- copyparty overwrite: PUT needs the `Replace` header (the `?replace` query only works for
  multipart POST) - found when the second publish saved `latest.txt-<time>.txt` beside the old
  one; the script's read-back check caught it.
- Verified (Linux + local copyparty): 0.2.0 from the zip (4 files, ranges) > restart > install >
  latest; then 0.3.0 changing one file: only that file's range was fetched.

## 2026-10-15 (10) - Claude (instance 2): online ports editable; joining through NAT loopback

User: 41790 can't be forwarded (ISP-restricted port?) - make the port editable, an entry for
both the host and the joiner. Then: the joiner shows up in the host's list briefly, then gets
"No answer from the host", while Wireshark shows the host answering.

- `rt_menu.c` Online screen: **Host port** (settings.ini `net_port`; locked while hosting) and
  **Join port** (online.ini `join_port`) rows: Left/Right steps, 2 types digits. The address row
  is the address only (`ip:port` typed or kept from before: the port moves to its row).
- `rt_net.c`: cause of the second problem: a joiner only accepted packets from the exact
  address it typed. Joining the host's public IP from the same network (or the same PC), the
  router delivers the HELLO but the host's answer comes back from its LAN address - dropped,
  so the joiner timed out and the host dropped the player 5 s later. HELLO now carries a nonce
  and WELCOME echoes it; a WELCOME with our nonce from another address makes that the host's
  address. Other foreign packets are logged once.
- Linux test hooks: `RT_MENU_ONLINE=1`, `RT_TYPE="sec:text"`.
- Verified: reproduced (join 127.0.0.2, host answers from 127.0.0.1: "joined", "left (no answer
  for 5 s)", "No answer from the host"); fixed: connected, the online game started and a
  rejoin through 127.0.0.2 loaded (fields identical to the host's: 24 hashes); a normal join
  still works; the port rows (frames checked): host 41790 -> 41792 with Right, join port 41789
  with Left, typed 5000 + Enter -> online.ini.

## 2026-10-15 (9) - Claude (instance 2): updates from the author's server

User: when the game starts it calls the user's server (copyparty) and downloads an update if
there is one. Boot order: safety warning > hold the Wii Remote sideways > **update screen** >
title; the screen shows a spinner while it checks, then a download bar.

- `runtime/rt_update.c` (new): background check at start (WinHTTP; curl on Linux), signed
  update list `latest.txt` (Ed25519, key built in), files by SHA-256 (`files/<hash>`), only
  changed files downloaded to `update/files/`, hash-checked; install = rename the running exe to
  `.old` + new exe in its place, the other files at the next start before anything opens them
  (`update_install_pending`), `.old` cleanup. Protected: disc/, nand/, online/, update/,
  settings.ini, wiimotes.ini, online.ini, profiles.json, update.ini.
- `runtime/rt_crypto.c` (new): SHA-256, SHA-512, Ed25519 verify (TweetNaCl's).
- `rt_lyt.c`: the boot's request for the title waits while `update_boot_hold()` says so.
  `rt_menu.c`: the update screen (boot-screen look: pale sky, striped panel, the game's font
  loaded at boot), 1/B skips a download; the version under the Options panel.
- `rt_net.c`: HELLO's build field is the game version; the host refuses a different one.
- `VERSION` (0.1.0), `tools/version_header.py` (out/rt_version.h: version, `update_url.txt`,
  `update_key.pub`), `tools/publish_update.py` (keygen / publish / upload to copyparty, refuses a
  version that isn't higher), `update_files.txt` (what is published). build.py: rt_crypto,
  rt_update, -lwinhttp. settings.ini: `update_check`.
- Verified on Linux against a local copyparty: download > restart > install > "latest" > title;
  skip mid-download; unreachable server; tampered list; protected path; damaged file; online
  version mismatch refused (same version still joins); Ed25519 vs RFC 8032 + 119 cases.
- Not yet: any Windows run of it. Not set yet: the user's server address and key.

## 2026-10-15 (8) - Claude (instance 2): title screen: staying in our menus no longer kills the players

User: staying in a title menu for a while, the running characters die and the title restarts.
Cause: the title is a real course and its course timer kept running while our menus (list,
Options, Who's playing?, Online) were up; after ~11 minutes "time up" killed everyone and the
title started over (reproduced headless: `dCourseTimeUp_c` after the list was left open).

- `rt_menu.c`: while our menus hold the title (camera not following), the course timer
  (`dStageTimer_c::m_instance` 0x8042A070, +4 mTimeValue) is held at its value; it runs again
  when the players are back. The parked players (off screen, right) are also held at their
  height, so a pit under them can't kill them either.
- `rt_det.c`: `RT_DET_UNPACED=1` (testing): deterministic runs as fast as they go.
- Verified: list left open for 59,400 frames (16.5 min of game time, unpaced) - no time up, no
  restart (before: time up at ~11 min).

## 2026-10-15 (7) - Claude (instance 2): player profiles, step 1

User: shift player data from the characters to player profiles; begin with profiles. Local for
now (a central user database maybe later); a unique ID per username, used later in multiplayer
saves for lives / coins / power-ups; profiles picked per controller on a new menu before the
online menu; contents: global + per-save stats, a 4-letter username shown only in multiplayer,
a preferred character preselected unless someone else got it first.

- `runtime/rt_profiles.c` (new): profiles.json (profiles + controller -> profile), IDs,
  create / edit, per-slot choice. docs/PROFILES.md.
- `rt_menu.c`: **Who's playing?** after Multiplayer (before Local / Online), a row per
  controller driven by that controller; new profile: username + preferred character.
- `rt_chars.c`: per-slot username / preferred character tables (save state); the character
  select preselects the preferred character when free. `rt_remotes.c`: `remotes_slot_raw`.
- `rt_net.c`: HELLO carries each joining player's username + preferred character; the host
  puts them into its game.
- build.py / build_linux_clang.sh: rt_profiles.
- Verified on Linux (deterministic, keyboard): profile created and remembered, preferred
  character preselected; online start still in sync. Stats are stored but not counted yet.

## 2026-10-15 (6) - Claude (instance 2): online menus, start at the character select, players can drop out and rejoin

User: Start the online game should take everyone to the character select; the host chooses the
game and save file first; no Online on the title list - press 2 > Singleplayer / Multiplayer /
Options, Multiplayer > Local / Online. Also: one player closing their game kicked everyone out -
it should try to reconnect before giving up.

- `rt_menu.c`: two-layer list (Local / Online under Multiplayer); the Online screen's host row
  "Choose the game and save file" continues into the multiplayer setup screens; at the
  character select a panel (players in, "2: Start the online game") starts it. The title's
  "back at the title" reset now waits a second (it fired while the game was still leaving the
  title and turned the multiplayer start into singleplayer). A notice shows why an online game
  ended. `rt_gamesetup.c`: `gs_char_select_ready`. `rt_remotes.c`: `remotes_local_slot`.
- `rt_net.c`: player map from the slots already in the host's game; away players (1.5 s)
  don't stall the others; gone players (BYE) can join again into the running game (a fresh
  state of the game, catch-up through the host's field history, SFREQ); clients wait 30 s for a
  lost host ("reconnecting"); only the host ends the game for everyone. `g_det_fast`: unpaced
  while catching up (`rt_core.c` det_pace). Test hooks RT_NET_QUIT_AT, RT_NET_BLIP_AT,
  RT_NET_ONLINE_SETUP.
- Verified on Linux, two/three copies over 127.0.0.1 (docs/ONLINE.md "Testing"). Windows build
  clean; still not run there.

## 2026-10-15 (5) - Claude (instance 2): save states; the online game starts in place (no restart)

User: Start the online game restarted every copy (the logs showed the restart worked, but it
looked like a crash and put everyone back at the boot screens); asked for no restart: the host's
running game is sent to everyone.

- **Save states** (`runtime/rt_state.c` new, docs/SAVE_STATES.md): fixed addresses for guest
  memory (0x500000000000) and an arena (ARAM + one stack slot per guest thread,
  0x600000000000); our own fibers (asm, SysV + Win64 with TEB stack bounds) replace Windows
  fibers / ucontext; the guest main thread runs on a fiber; guest thread records are a static
  pool; ARAM moved to the arena. State = MEM1 + the mod area up to 0x82000000, MEM2, locked
  cache, ARAM, the state sections (`RT_STATE_FILE` on 18 runtime files, `RT_STATE` /
  `RT_LOCAL` single variables), the fiber stacks. `rt_snap_take` / `rt_snap_load`.
  rt_ios: FILE*s are per process (reopened by open id). rt_remotes: GC port timing + menu
  buttons in the state. rt_det: hash covers 32 MB of MEM1 now (the mod area).
- Builds: the runtime needs clang (`#pragma clang section`); build.py links at a fixed base
  without ASLR, `-fno-stack-protector`, adds rt_state; the Linux test build uses clang for the
  runtime, `-fno-stack-protector -fcf-protection=none`, non-PIE.
- **Online game in place** (`rt_net.c` rewritten from "online game" down, docs/ONLINE.md):
  files -> READY -> the host takes S0 at a field (`rt_det_enable` first) -> SNAP -> each
  client freezes, keeps a state of its own game, fetches S0's page hashes, asks for the pages
  that differ (WANT), fetches them (PAGES, LZ-compressed), checks every page and loads S0.
  No restart, no session.ini / NSMBW_ONLINE any more. Ending: everyone loads their own game's
  state back. Window title shows loading progress.
- `rt_core.c`: `rt_det_enable` (virtual clock from now on, no jump), `rt_settings_reload`,
  `det_pace_reset`; RT_POLL_LOG / RT_DEC_LOG debug logs. `rt_det.c`: `RT_SNAP_TEST`,
  `RT_SNAP_SAVE`, `RT_SNAP_LOAD`; calls `net_snap_point`.
- Verified on Linux: in-process state replay identical for 1000 fields; a state written by one
  copy and loaded by another identical for 1758 fields; online start in place over 127.0.0.1
  (client needed 0.2 MB of 117 MB), both games identical for 4,198 fields; leaving brings both
  back to their own games. Windows build clean (sections rtd / rtb / rtl present, base
  0x140000000, no dynamic base) - not run on Windows yet: the fibers / TEB handling there are
  new and untested.

## 2026-10-15 (4) - Claude (instance 2): online game - one session folder per copy

User: the game "crashes" on Start the online game. Their log: host and client were two copies
in the same game folder (join = localhost). Both wrote `online/session.ini`; the host's copy was
written last, so both copies restarted as the host - the second one couldn't open the port
("the host's port is in use"), ended the session and restarted normally, while the first sat
waiting for a player that never came (no new frames: looks frozen). Both copies also wrote the
same nsmbw_log.txt, which hid what happened.

- `rt_net.c`: sessions live in `online/<name>/` (`host`, `p1`, ...), chosen by
  `NSMBW_ONLINE=<name>` that the restart puts in the new copy's environment (`_putenv_s` /
  `setenv`); a normal start is never in a session (a stale session.ini can't hijack it). Why a
  session ended shows on the Online screen after the restart (`online/last_message.txt`).
  `rt_session_dir()`; `rt_core.c` reads the session's settings.ini from there.
- `rt_plat_win.c`: a session's log is `online/<name>/nsmbw_log.txt`; `plat_title_note`: the
  window title says "Online (host)" / "Online" / "Online: waiting for the other players (N s)" /
  "Online: OUT OF SYNC" (Linux: logged).
- Verified: two copies in one folder (Linux, `localhost`): lobby -> both restart into their own
  session -> identical over 2102 fields. Windows build clean. No real crash was found in the
  log (the copy that would have shown one had its log overwritten); if it still crashes, the
  session's own log now keeps it.

## 2026-10-15 (3) - Claude (instance 2): deterministic mode; online game (lockstep)

User: go with rollback; first the lockstep session. Also asked to note in TODO: smoother online
flow, host-decided settings, several local players per PC, the 4:3 overhaul (more playfield
vertically, not zoom), and an update check at start (download a newer version from the user's
server).

- **Deterministic mode** (`rt_det.c` new, `rt_core.c`, `rt_hw.c`, `rt_gxrender.c`): virtual
  clock for everything the game can see (`rt_emu_ns`), inputs latched per video field, a
  state hash without the renderer's writes; testing env vars in `rt_det.c` / docs/ONLINE.md.
  Verified identical runs (null, ~4000 fields, with random host delays) and GL vs GL; null vs GL
  drifts (TODO).
- **Online game** (`rt_net.c`): host offers (session id, delay, clock, player map, files),
  clients download + verify, READY / GO, everyone restarts into `online/session.ini`. In the
  session: lockstep through the host (SINPUT / SFRAME, input delay `net_input_delay` = 3),
  SHASH every 120 fields + OUT OF SYNC, 10 s timeout / BYE ends it (restart). Details in
  docs/ONLINE.md.
- `rt_nand_root()` (`rt_ios.c`, `rt_mod.c`, `rt_core.c`): the session plays on
  `<game>/online/nand`. `rt_core.c` settings: the host's in a session, except display / sound /
  network keys (`setting_is_local`).
- `rt_remotes.c`: in a session the game's players are the session's (emulated remotes / GC
  ports with the agreed buttons); local remotes are pumped for their buttons + shake;
  `remotes_local_buttons(0)` falls back to the keyboard when nothing is set up.
- `rt_menu.c`: Online screen gets **Start the online game** (host) and, in a session, only
  **Leave the online game**. `rt_plat_win.c`: closing the window sends BYE (`net_shutdown`).
- `build.py` / `build_linux_clang.sh`: `rt_det`. `rt.h`: `plat_idle_wait` declared.
- Verified on Linux with two instances over 127.0.0.1 (docs/ONLINE.md "Testing"). Windows
  build clean. Not yet tried on two real PCs.
- TODO: update check / auto-update (user), session follow-ups.

## 2026-10-15 (2) - Claude (instance 2): online play, phase 1 (connection)

User: begin online multiplayer - direct IP, the whole session, host-authoritative with
client-side prediction / reconciliation / snapshot interpolation (asked mid-way whether there's
a more efficient way to minimise input lag and visual errors: answered - rollback; pending the
user's choice before phase 2).

- `runtime/rt_net.c` (new): UDP sockets (Winsock / BSD, non-blocking, polled from
  remotes_update), HELLO / WELCOME / REJECT / INPUT (8 frames redundant) / STATE / PING / PONG /
  BYE, jitter-buffered input playback on the host, timeouts. `build.py` links ws2_32.
- `rt_remotes.c`: slot kind A_NET (online player): buttons from rt_net, counts as in use
  (emulated remote / GC port), can't be reassigned in Controls; `remotes_local_players` /
  `remotes_local_buttons` (what a client sends); `remotes_slot_emulated` replaces the keyboard
  test in `rt_wiimote.c`.
- `rt_menu.c`: title list gets **Online** (Singleplayer / Multiplayer / Online / Options); Online
  screen (host / join with a typed address / leave, status, players + ping). `rt_plat_win.c`:
  WM_CHAR queue (`plat_text_char`).
- Docs: `docs/ONLINE.md`. Verified: loopback host + client (details there). Windows build clean.

## 2026-10-15 - Claude (instance 2): Wii Remote slots by connection; SMB1 Mario's abilities, goal pole, vines, sheets, sounds

User: forget the remotes' positions (keep their IDs; a connecting remote takes the next free
slot). SMB1 Mario: remove sliding on slopes, crouching while small, carrying, riding Yoshi (+ a
Yoshi turned into SMB1 Mario); goal pole (animation, offset, drawn over the pole, SMB1 slide
speed and logic, walk to the castle and vanish, past the other players' poses); vines (left /
right only, broken sprites); stray pixels in the swim loop; no particles (water, map); big
Mario sheet; sound effects (ui/retro/mario/sfx) only for SMB1 Mario's actions.

- **Remotes** (`rt_remotes.c`): assignments are for the session; a remote that connects takes
  the lowest slot without the keyboard or another remote and frees it when it disconnects.
  `wiimotes.ini` keeps `keyboard = N` and `known = <address>` (older `slotN = ...` files are
  converted). Controls' Left/Right still moves things for the session; a remote set to None sits
  out until it disconnects. Verified headless (fake remotes).
- **SMB1 Mario** (`rt_retro.c`, details in `docs/RETRO_MARIO.md` items 7-12): changeState
  refusals (Slip, small Crouch/SitJump, LiftUp, RideYoshi), spinLiftUp and Yoshi's ride-on say
  no, Yoshi models never retro; effects dropped during its update and its map smoke; goal pole
  script from the disassembly; vines held left/right of the vine; sheet frames in their own
  texture cells (no neighbour slivers; empty boxes missing); super.png for big forms; 4 swim
  pictures x kick.
- **Sounds** (`rt_sfx.c`, new): WAV loader/resampler and a mixer in the audio DMA path
  (`rt_hw.c`); hooks on detail_StartSound / detail_HoldSound / startMissBGM. `build.py` now also
  deploys `.wav` files from `ui/`.
- Verified headless (Linux): crouch refused while small; jump / stomp / swim / coin / bump /
  death sounds and the NSMBW player sounds silenced (RT_SE_LOG / RT_SFX_LOG); the goal (World
  1-1 from its midway pipe): slides 1 px/frame, waits, swings, walks to the castle (x 6944) and
  vanishes - frames dumped and looked at (drawn over the pole); sheet cells dumped (swim row
  clean, big sheet's empty boxes skipped); swimming in 4-2 makes no effects. Windows build clean.
  Not verified: vines, Yoshi, carrying, map smoke with a retro map player.
- **Open: underwater bubbles** - the bubbles near SMB1 Mario after a swim stroke remain. They are
  not effects (they stay with `RT_NO_EFFECTS=1`, which drops every effect, incl. EGG::Effect::create)
  and not made by the player's own update; probably the water (AC_BG_WATER / dWaterEntryMng_c).

## 2026-10-14 (later still, 6) - Claude (instance 2): Wii Remotes switch off when the game closes

User: closing the game should cut the remotes' Bluetooth link and turn them off.

- `rt_plat_win.c` `wm_shutdown()`: on window close (before `ExitProcess`) and after a crash
  (once the crash box is closed), every Wii Remote's link is dropped with
  `DeviceIoControl(radio, IOCTL_BTH_DISCONNECT_DEVICE 0x41000C, &address, 8)` (no public API;
  DS4Windows turns its controllers off this way). A remote powers itself off when its link
  goes; it stays paired and any button reconnects it next time. Remotes: the ones we have open
  plus any other connected "Nintendo RVL-CNT-01" Windows lists, on every radio. Log:
  `[wm] closing: Wii Remote ... off -> ok/failed`. Not on Options' restart (the new copy keeps
  them). Windows build clean; not testable here.

## 2026-10-14 (later still, 5) - Claude (instance 2): pairing failed with error 1244

User: pairing now runs but always says "failed". Log: SYNC PIN -> 1244 (ERROR_NOT_AUTHENTICATED),
then the 1+2 PIN -> 1244, for two different remotes. The PC's Bluetooth address is
E8:48:B8:C8:20:00, so the SYNC PIN (address reversed) is 00 20 C8 B8 48 E8 - it **starts with a
0 byte**. `BluetoothAuthenticateDevice` takes the PIN as a WCHAR string, which ends at that 0,
so Windows gets an empty PIN. (The 1+2 PIN is simply the wrong one for a remote in SYNC mode.)

- `rt_plat_win.c`: `pair_sync()` - PIN without a 0 byte: Dolphin's text call first; otherwise
  (or if that fails) `BluetoothAuthenticateDeviceEx` with a `BluetoothRegisterForAuthenticationEx`
  callback that answers with the 6 bytes as binary (`BLUETOOTH_PIN_INFO`, length 6). That call runs
  on its own thread with a 20 s limit (an earlier version of it hung) and logs whether Windows
  asked for the PIN. The 1+2 PIN is no longer tried after SYNC (Dolphin: the wrong method makes
  Windows drop the remote). As Dolphin does, the HID service is switched on even if pairing
  failed, so the remote still works this session ("Connected ... for this session only").
- Windows build clean. Not testable here; the log lines `[wm] pairing:` show each step.

## 2026-10-14 (later still, 4) - Claude (instance 2): crash at boot (Wii Remote lock)

User's log: crash right after "[remotes] player slot ..." - access violation writing 0x24 inside
ntdll. `remotes_update` now asks `plat_wm_pair_last` (new, last entry) before anything else
on the platform side, and that entered `wm_lock` before `hid_load()` had initialised it (every
other `plat_wm_*` call loads first). Fix: `plat_wm_pair_last` calls `hid_load()` first.
Windows build clean.

## 2026-10-14 (later still, 3) - Claude (instance 2): Wii Remotes - pairing fix, fixed player slots

User: after "Remove device" in Windows, the game found the remote and said "Pairing..." but it
never paired. Wanted: remotes mapped to fixed player slots (only recognised there), a way to
move a remote to another slot, and to switch a slot between keyboard and remote so keyboard +
1-7 remotes works. Details: **`docs/WIIMOTES.md`**.

- **Pairing** (`rt_plat_win.c`): the user's log stopped at "Pairing Wii Remote ..." -
  `BluetoothAuthenticateDeviceEx` with the `BluetoothRegisterForAuthenticationEx` callback never
  returned. Now exactly Dolphin's way: `BluetoothAuthenticateDevice(NULL, radio, &info, pin, 6)`
  with the 6 PIN bytes one per WCHAR (SYNC: PC address reversed; then 1+2: the remote's own),
  then `BluetoothEnumerateInstalledServices` and `BluetoothSetServiceState(HID)`. Before
  looking, remembered remotes that are neither connected nor authenticated are removed (Windows
  can't use them). Inquiry `cTimeoutMultiplier` 2. **Not tested on hardware** (none here).
- **Player slots** (`rt_remotes.c` rewritten): each slot holds the keyboard, a remote (by
  address) or nothing - `wiimotes.ini`: `slotN = keyboard | <address> | none`, plus
  `known = <address>` for remotes with no slot now. A remote plays only in its own slot; a
  connected remote with no slot is not used (no lights; the Controls screen says so). A remote
  just paired from Controls takes the first free slot (`plat_wm_pair_last`). The keyboard is in
  exactly one slot (slot 1 by default / for old ini files); it can't be set to none, only moved.
  While slot 1 has nothing connected, the keyboard drives player 1 too (the game's own menus
  listen to player 1). Our menus read player 1's device + the keyboard; the game gets each
  slot's own buttons (`remotes_game_buttons`; `rt_wiimote.c` uses it for real and emulated
  remotes; GC ports 5-8 take the keyboard too). Emulated remote 1-3 connects while the keyboard
  is in its slot.
- **Controls screen** (`rt_menu.c`): rows "Player N | lights | < Keyboard / Wii Remote addr /
  None > | Connected / Not connected"; Left/Right cycles a slot through None, Keyboard and every
  known remote (sorted by address, so the order doesn't change as they move) - taking one from
  another slot swaps the two; replacing the keyboard moves it to the first empty slot. 2 =
  Forget on a remote's slot. Line for connected remotes without a slot.
- Verified headless (fake remotes): defaults (keyboard slot 1, remotes unassigned, no lights);
  a "paired" remote (`RT_FAKE_WM_PAIRED`) takes slot 2; keyboard in slot 3 with remotes in 1, 2,
  5, 6 -> players CHAN_0, CHAN_1, CHAN_2 (keyboard), CHAN_GC_0, CHAN_GC_1; keyboard presses go
  to slot 2 only (`RT_BTN_TRACE`), the title's "2" still reaches player 1; cycling/swapping in
  the Controls screen (dump checked). Windows build clean.
- New test switches: `RT_FAKE_WM_PAIRED="frame:remote,..."`, `RT_BTN_TRACE=1`.

## 2026-10-14 (later still, 2) - Claude (instance 2): Wii Remotes - pairing for good, up to 8

User: real remotes paired in Windows had to be paired again every session. Wanted: a pairing
dialog in the game, pairing once (the remote recognises the game after a reboot and reconnects
at once), and 8 remotes; lights for players 5-8 as combinations (my pick: 1100 0110 0011 1001).
Full description: **`docs/WIIMOTES.md`**.

- **Pairing** (`rt_plat_win.c`, Windows Bluetooth API from bthprops.cpl at runtime): Options >
  Controls > "Pair Wii Remotes" looks for remotes in pairing mode; SYNC button -> PIN = the PC's
  address reversed (binary, via `BluetoothRegisterForAuthenticationEx` callback), so the remote
  stores the PC and reconnects by itself with any button; 1+2 -> guest pairing (remote's own
  address), session only. Stale pairings are removed first; the HID service is switched on.
- **8 remotes on Windows**: HID scanner every 2 s, per-device reader/writer threads, address from
  the HID serial string. New platform API `plat_wm_*` (rt.h) replaces `plat_wiimote_*`.
- **`rt_remotes.c` (new)**: 8 slots, remembered by address in `wiimotes.ini`; slots 1-4 = the
  game's Wii Remotes, 5-8 = GameCube ports; player lights (LED reports to remotes 1-4 get them
  put in); GC translation of buttons + accelerometer shake.
- **`rt_wiimote.c` rewritten for 4 remotes** (handles 0x100-0x103, per-remote L2CAP/HID state and
  passthrough). Remote 0 always (keyboard); 1-3 only while a real remote is in their slot, then
  disconnect (HCI Disconnection Complete, reason 0x13) when it leaves. They connect one at a time
  in slot order (the module numbers players in the order their controllers appear).
- **SYSCONF BT.DINF**: 4 registered + active remotes (patched at boot by `rt_ios.c`; `make_nand.py`).
- **`rt_si.c` (new)**: the serial interface with 4 GameCube controller ports (type, origin, poll,
  per-field polling, interrupts). `hle_SITransfer`: a port's first transfer counted the delay from
  time 0, but the system time is negative early here (0x800030D8 = -1) -> ~70 s wait; fixed.
  The module's GC mapping was measured (docs). GC ports appear after the Wii Remotes, in order.
- `rt_menu.c`: Controls screen redone: 8 player rows (lights, Connected / Not connected,
  address), Pair / Stop Pairing with the pairing status, 2 on a row = Forget (confirm), keyboard
  keys in one line. `menu_owns_input()`: players 2-8 get no buttons while our menus / the title
  are up.
- Test switches: `RT_FAKE_WM`, `RT_FAKE_WM_SCRIPT`, `RT_FAKE_WM_GONE`, `RT_GC_TEST`,
  `RT_CONTROLS_AT` (docs).

Verified headless (fake remotes): 8 remotes -> players 1-8 in slot order, lights right, leaving
remotes shut down; multiplayer select: players 2, 5, 6 join and choose; singleplayer unchanged;
Controls screen screenshot. **Not verified: real Bluetooth pairing / real remotes (no hardware
here)** - the user's test decides; logs: `[wm] pairing: ...`, `[wm] Wii Remote ... connected`,
`[remotes] ... -> slot n`.

---

## 2026-10-14 (later still) - Claude (instance 2): singleplayer character select - taking a choice back

User: once a character was chosen, the player couldn't drop out: a Back button appeared next to
Start Game and it went to the previous menu. Wanted: no Back button, the "Back 1" label instead,
Start Game centred and only there once a character is chosen; 1 after choosing takes the
character back, 1 again goes to the previous menu. Multiplayer is unchanged (Start Game / Back).

`runtime/rt_gamesetup.c` (singleplayer, setup scene only):
- `hle_mod_fn_2_343F0` (new, dNumberOfPeopleChange_c::executeState_ButtonSelect): the cursor
  stays on Start Game (left/right masked for the module's code). 1 (`dGameCom::chkCancelButton`)
  takes the choice back: the button animations (groups 2-7: inYesNoButtons yes/no/guide,
  onYesNoButtons) are reset to their first frame (`resetAnim`, 0x800C93D0), what the button
  phase hid comes back (indicator panes +0x21C/+0x220, 2D players +0x266, np +0x688 -
  remembered when NowEntrantRecruit hands over), the cards' exit flags clear, the card does
  what 1 on a decided card does (PlayerDisp: back SE, enter count - 1, state PlayerExitWait
  0x80990D80) and the select goes back to NowEntrantRecruit (0x809945B0). The next 1 there is
  the existing "back to File Select".
- `hle_fn_2_2DF0` (new, rel dCharacterChangeSelectBase_c::executeState_SelectStartWait - the
  "Join" wait after a drop): that card comes straight back (ButtonExitAnimeEndWait 0x80990AC0,
  what 2 starts there).
- `rt_chars.c` `chars_keep_choice(slot)`: readyContents (wrapped) puts the card back on the
  character that was taken back (the module's readyContents resets the cursor to the slot's
  default, Mario).
- Layout (`gs_layout_calc` / `gs_layout_draw`): P_noBase_00 hidden; P_yesBase_00 moved to x 0
  (centre) and hidden until the character is chosen; N_back ("Back 1") kept visible during the
  button phase - the guide group's inYesNoButtons animation squashes it (scale y 0), so its scale
  / alpha are set before CalculateMtx.
- Test switch: `RT_SELECT_SCRIPT` takes `1` (button 1).

Verified (GL, headless): choose Luigi -> Start Game alone in the middle, "Back 1" shown -> 1:
the figure leaves, the card comes back on Luigi, no Start Game -> choose again -> 1 -> 1: File
Select. The card passes briefly through the module's "Drop" / "Join" indicator while it returns.
**game\nsmbw.exe not replaced**: too big for the bridge (~59 MB, 30 MB limit) and the PC shell
can't mount the folder - the user rebuilds.

---

## 2026-10-14 (late night) - Claude (instance 2): Retro Mario - no 3D model anywhere

User: "in a lot of areas he uses the regular mario model, this should never happen - all
instances of the NSMBW mario model should be replaced with the plane of mario's graphic". Rules:
world map - the plane faces only left or right, keeping the last left/right while moving up or
down; character select - left or right by the player's input, no spinning like the 3D models.
Also (process): **always replace game\nsmbw.exe directly** (no update zip); if it can't be
written (in use / permissions), say so and the user rebuilds. This time it could not be: the
bridge moves files up to 30 MB (nsmbw.exe is ~59 MB) and the PC shell couldn't mount the folder
to unpack a zip there - sources were deployed, the user rebuilds (`.xfer\nsmbw_exe.zip` holds the
built exe zipped, as a fallback).

- Levels already drew the sprite (dAcPy_c::draw). Everything else - the world map player, the
  character select figure, anything else - draws player models through `dPyMdlMng_c::draw`,
  now hooked (`draw__11dPyMdlMng_cFv`; `__dt__11dPyMdlMng_cFv` drops the per-model draw proc
  with the model). Survey (who draws models): levels `fn_80145A60` (player number in model
  +0x150, scene type +0x180 = 0); select figures `fn_2_157E30` / `DoSpecialDrawing1` (scene 2,
  +0x150 = player type); world map `fn_2_195320` (scene 1, +0x150 = player type).
- A retro model: the slot whose `mPlayerType` is that type plays SMB1. It is drawn as the sprite
  quad, built in view space (always faces the camera), at the model matrix's position, as big
  as the model (matrix scale / the model's level scale at 0x80370DA8). Form from
  `mPlayerMode[type]`. Picture: walk cycle when it moves, jump when it rises/falls, else stand.
- Facing: select - `chars_select_facing(slot)` (rt_chars.c: the last left/right the slot pressed,
  polled while the figure shows, plus SelectWait's card moves); elsewhere - the model's forward
  in view space (|x| > 0.35 switches, else kept: up/down keeps the last left/right).
- rt_retro.c: the quad drawing is now `sprite_draw()` (shared by the level and model procs).

Verified (GL, headless): select -> Retro Mario -> the figure that jumps onto the card is the
sprite, flips with left/right; world map: the sprite stands on the 1-1 node, turns left/right with
the presses (the new save has no open path, so no walk along a path was seen); 1-1: sprite as
before. Not changed: the 2D head icons (HUD "x05", course-in, file select) are still NSMBW's.

---

## 2026-10-14 (night) - Claude (instance 2): select cards - original slide back, real cause fixed

User: the portraits were "super messed up" after the last two builds; they want the card's
original behaviour (the slide between characters), only rendered correctly - not changed.

- **Reverted** the "one portrait at a time" change of the evening entry: dispNextConPicture
  (`rt_chars.c`, `mod_fn_2_5AB0`) is the first per-side version again (each face shows its own
  character's picture, nothing hidden), and the face pinning (`chars_layout_calc`, called from
  `rt_menu.c`) is gone.
- **The real cause**: the card is clipped by the game - the select base computes the contents
  layout's m2d clip from the base layout's `N_scissorArea_00` (original rel fn_2_2170 ->
  fn_2_2810 -> fn_800C9680, rect at contents+0x100..+0x110) and fn_80007010 sets it with
  GXSetScissor. `rt_aspect.c` hle_GXSetScissor widened *every* m2d clip (caller 0x80007198) to the
  whole screen in 4:3 mode (the default), meant for the letterbox bands of the world map HUD /
  prompts - so the faces sliding past the card showed beside it. Now only a clip that spans the
  picture's width (x 0, w >= 600) is widened; the card's (e.g. 262,153 116x126) is kept.
  In 16:9 mode the clip always worked.
- `RT_DUMP_FROM=n` (Linux test build): first frame `RT_DUMP_EVERY` dumps.

Verified (GL, llvmpipe, 4:3, `RT_SELECT_SCRIPT`, dumps every 2 frames): Mario -> Luigi -> Ala Gold
-> back, the faces slide inside the card window, nothing beside it; custom portraits slide the
same way (Buckenberry -> Toadette -> Toadsworth -> Daisy -> Rosalina -> Goomba). The world map's
letterbox clip (0,63 640x330) is still widened in 4:3 (`RT_VPLOG=1` logs every m2d clip as the
game asked for it: "m2d clip (4:3, as asked)"). After deciding, the card's clip shrinks to
nothing with its animation (the game's own; the old hack had hidden that too).

---

## 2026-10-14 (evening) - Claude (instance 2): mod models + portraits, one portrait at a time, retro fixes, singleplayer state

User feedback on the previous build, all handled:

- **Character select showed two portraits** (the card's and the next one beside it). The card
  has two faces - side 1 slides the next character in during a move - and the layout has no
  mask, so whatever is beside the card shows. `rt_chars.c` dispNextConPicture: only one face
  is visible (the incoming one while moving, else the card's).
- **Models from the mods in game\mod** (zips, as downloaded): new `tools/import_char_mods.py`
  (build.py runs it when a zip there is newer than `game\mod\characters\.imported`; needs
  Pillow for the portraits - build_windows.bat installs it) extracts the brres files to
  `game\mod\characters\` and portraits to `ui\characters\`. New `runtime/rt_charmdl.c` loads
  them into guest memory (0x81C00000+, ResFile::Init + Bind like the game's loader) and hooks
  dPyMdlMng_c::construct (`mod_fn_800D6D10`: Purple Toadette's body becomes a Toad-type model),
  dPlayerMdl_c::loadResources (`fn_800D32F0`: the model / P_rcha / anim ResFiles are pointed at
  ours) and dKinopioMdl_c::setColorType (`mod_fn_800BCEE0`: texture frames). Result:
  Toadsworth = Black Toad body with the Toad addon's textures; Daisy = Orange Toad body,
  Rosalina = Purple Toadette body, both the Rosalina/Daisy mod's model with its own animations
  (its P_rcha has dress/skirt tracks but no CapPosition - Mario keeps the disc's). Roster bodies
  changed accordingly (rt_chars.c). Not covered: world-map figures, voices, HUD icons, the mods'
  Kamek code. Goomba's model needs a 9th body.
- **Portraits from the mods**: Daisy, Rosalina (that mod's select portraits), Goomba (the Goomba
  mod's), Toadette and Toadsworth (no portraits exist anywhere: their HUD icons, scaled).
- **Retro Mario**: jitter - SMB1's whole-pixel steps (1, 2, 1, 2...) judder against NSMBW's
  camera; the player now moves by the SMB1 speeds (x / 16; y + fraction / 256), same distance
  on average. Slopes / tilting floors - the old "push down 2 + |dx| every frame" clipped into
  them; on the ground NSMBW's own step now moves the player at the SMB1 speed (it follows slopes
  and moving floors like the other characters). Particles - the six dEf::createPlayerEffect
  functions do nothing for a retro player. New test switch `RT_RETRO_STAR=1` (invincible).
- **Singleplayer lives / power-up follow the save file, not the character** (multiplayer: per
  character). The save keeps them in Mario's slot (type 0); picking a character in singleplayer
  copies them to its body (initDecidedCharacter), saving copies them back (prepareSave hook,
  `mod_fn_800E1690`).

Verified headless with screenshots: one portrait while walking the cards; Toadsworth, Daisy and
Rosalina on the select (3D figure) and Rosalina in 1-1 with animations; Retro Mario running and
jumping across 1-1's slopes with smooth positions (log), no dust.

---

## 2026-10-14 (later) - Claude (instance 2): character select with the 12-character roster

User: more than 8 characters regardless of player count (12 now, more later); keep NSMBW's
cards in the select (left/right walks the list); title screen shows the first five; the extra
characters had no portraits. Details: `docs/CHARACTERS.md`.

- `runtime/rt_chars.c` (new): the roster (id, name, body = one of the 8 player types, style),
  the character each player slot picked (`chars_of_player`, `chars_is_smb1`), and the character
  select: SelectWait / calcContentsIcon / isCharacterLocked / initOption / initDecidedCharacter /
  readyContents / dispNextConPicture native or wrapped (`hle_funcs.txt`), the arrows' range
  mapped. `mOption` now runs 4 .. -7 (character = 4 - mOption); the card's icon value is
  `100 + 2 * character + taken`.
- Portraits: `ui/characters/<id>/portrait.png`, swapped into the card's Mario picture pane
  through its TexMap (material + 0x6C, pointer at material + 0x40 - found by dumping, see
  `RT_CHARDUMP=1`). Placeholders with the name: `tools/make_char_portraits.py` (never
  overwrites a painted one). Textures at 0x81A00000+; `rt_gxrender.c` texture range up to
  0x82000000.
- Bodies: only 8 types exist, so Goomba and the retro characters borrow a free one (retro
  prefers Mario's); a character is "taken" if another decided player has it or its body.
- Retro Mario (SMB1) is chosen in the select; the temporary Options row is gone (`retro_mario
  = 1` in settings.ini still forces it for every Mario - testing). `rt_retro.c` asks
  `chars_is_smb1(player)`.
- Title screen: `chars_title_players()` (from rt_title.c's resWaitProc2 wrapper) - entries for
  the first five characters only.
- build.py / build_linux_clang.sh: + rt_chars (build.py's deploy copies `ui/` with
  subfolders, so `ui/characters/` goes to game\ui too).

Verified headless with screenshots: cards walk Mario -> ... -> Retro Mario (SMB1) with the
layout's pictures for the first four and the placeholders after; arrows hide at both ends;
picking Retro Mario (SMB1) gives an SMB1-moving sprite Mario in 1-1 with retro_mario = 0;
title shows Mario, Luigi, the two Toads and Toadette.

Open: per-character lives/power-ups, more than 8 types, save format v2, models; the retro
character's card shows Mario's 3D figure after deciding.

---

## 2026-10-14 - Claude (instance 2): Retro Mario (SMB1) playable; character list planned

**Retro Mario (SMB1) works in the game.** Turn on **Options > Mario: SMB1 (retro)** (setting
`retro_mario`, applied at once): every Mario is then an NSMBW player that moves with the SMB1
physics and is drawn with the user's sprite sheet. Details, hooks and test switches:
`docs/RETRO_MARIO.md`. Files:
- `runtime/rt_retro.c` (new): the glue. Hooks the player's speed/position step
  (`fn_80056E40`), executeMain (`fn_801438A0`, collision fed back), setJump (`dAcPy_vf3FC`,
  stomps/springs), draw (`fn_80145A60`, sprite instead of model); returns "no" from the wall
  slide start (`fn_801285D0`), ground pound (`fn_801266B0`) and spin jump
  (`fn_8005E780`, except with the propeller suit).
- `runtime/smb1_physics.c/.h`: + `smb1_graphics()` (PlayerGfxHandler/AnimationControl: the
  picture per frame, incl. swim kick feet), FrameCounter/PlayerAnimTimer. Tests added to
  `tools/test_smb1_physics.c` (all pass).
- `runtime/rt_core.c` + `rt.h`: `rt_register_native(addr, fn)` - give a native function a
  guest address, so guest code can call it through a vtable (used for the sprite's draw
  proc); setting `retro_mario`.
- `runtime/rt_gxrender.c`: texture address range up to 0x81A00000 (the sprite sheets live at
  0x81900000, after the module).
- `runtime/rt_menu.c`: Options row "Mario: NSMBW / SMB1 (retro)" (window a bit taller).
- `runtime/rt_plat_linux.c`: `RT_RETRO_SCRIPT` buttons reach the game (testing).
- `ui/retro/mario/`: the user's painted `small.png` + frame positions measured from it in
  `frames.json` (row 2 holds 8 swim frames: 3 SMB1 swim pictures with and without kicking
  feet + 2 spare, then climb 1-2). `tools/make_retro_templates.py`: 8 swim frames; keeps the
  entry of a painted sheet; refreshes sheets that are still only white boxes.
- build.py / build_linux_clang.sh: + rt_retro.

Verified headless (Linux build, World 1-1, screenshots): walks, runs, jumps with the SMB1
numbers (log: x speed up to 0x28, jump arc), lands on the ground, drawn 1 tile wide with the
user's pictures, dies from walking into a Goomba (NSMBW's damage works on it).

**Character list** (user, 2026-10-13): more than 8 characters regardless of player count - 12
now, more later; first five on the title screen. Plan in `docs/CHARACTERS.md`, roster and
steps in `TODO.md`. Not started in code yet. The missing portraits for characters 5-8 in the
character select are not a port bug: the module has no portrait art for them (see
CHARACTERS.md).

Note for the other instance: the cloud workspace of this session was reset once mid-work;
everything above was rebuilt from the session log and is on the user's PC now.

---

## 2026-10-13 (retro) - Claude (instance 2): title button fix; Retro Mario started

**Title bug** (user): any button but 2 started singleplayer at once. The game's title takes
any button as "start" and our menu only caught 2. Now on the title (`T_IDLE`, and while the
title is loading) the game gets no buttons except HOME, and 2 or A open our list
(`rt_menu.c`).

**Retro Mario** - plan, decisions and known engine fields: `docs/RETRO_MARIO.md`; progress in
`TODO.md`. Done so far:
- `runtime/smb1_physics.c/.h`: SMB1's player movement, translated routine by routine from the
  disassembly the user put in `../SMB1Disassembly/SMBDIS.ASM` (original variable names,
  8-bit arithmetic, tables, quirks such as the shared X move-force byte). Engine-agnostic: the
  host feeds buttons and collision results. Compiled into the game, not used yet.
- `tools/test_smb1_physics.c`: walk/run top speed 1.5/2.5 px per frame, standing jump 66 px,
  running jump 83 px, terminal fall 4, no air friction, skid - all pass.
- Sprite sheet templates for the user to draw on: `ui/retro/mario/{small,super,fire,ice,
  propeller,penguin,mini}.png` + `frames.json` + `guide/` (`tools/make_retro_templates.py`;
  never overwrites a sheet). build.py now copies `ui/` with subfolders (not `guide/`) and keeps
  a copy in game\ui that is newer than recomp's.

---

## 2026-10-13 (last) - Claude (instance 2): menu freeze fix, author name out of the source, title players, TODO.md

New: **`TODO.md`** - shared list for the user and both Claudes (user's next items: Wii Remote
dialog for up to 8 remotes, retro Mario/Luigi, audio settings, settings in the pause menu).

### Multiplayer: 1 on the character select froze the menus
User report: Multiplayer -> file (or Free-for-All / Coin Battle) -> character select -> 1 ->
no more input. Reproduced headless: pressing 1 (or 2 on a file card) **during the 30-frame
fade-in**. Leaving asked the fader for a fade-out, but `mFaderBase_c` only starts one from
fully shown, so it was refused and the code waited for black forever with input off
(`np_exit_wait`; same trap in the scene's FadeOut state). Fixed in `rt_gamesetup.c`:
`screen_black()` re-asks for the fade-out once the fade-in is over; and game input is held
while the fader isn't fully shown (`fader_seen`, updated by the scene's state functions).
Plus, in `rt_menu.c`: a button the game didn't get when pressed (our menus had it, or it was
held during a fade) stays hidden until released (`menu_filter_buttons` wrapper) - a 1 held
past the end of the title menu's fade used to reach the game as a start press.

### The module author's name is out of the source
User request: no reference to the module's author by name in the source. Done everywhere
(code, comments, tools, docs, file names, NATIVE_PORT.md, this file), except
**`THIRD_PARTY_NOTICES.md`**, kept on purpose: the module is MPL-2.0, which requires telling
recipients of the executable where its source is - that file says it (license, source, release
address). Mechanics:
- `tools/fetch_mod.py` reads the release address from THIRD_PARTY_NOTICES.md ("- Release:"),
  keeps the zip as `mod/upstream-release.zip` (an old copy under another name is found by its
  SHA256), writes the archive as **`mod/nsmbw-8p.arc`** (served to the game as `/nsmbw-8p.arc`,
  `MOD_ARC_NAME` in rt.h) with `Info/Version.json` renamed to `{"nsmbw-pc": "v0.6.5"}` (log:
  `Project: nsmbw-pc v0.6.5`).
- `tools/link_mod.py` `rename_strings()`: the module's save file names become
  `nsmbw-save*.json` / `nsmbws.json` in linked/d_project.bin (same length or shorter, nothing
  moves), and its crash screen's issue-tracker link becomes "see nsmbw_log.txt". So the
  `save_alias` table and `ios_migrate_save_names()` in rt_ios.c are gone (the user's save was
  already migrated), and `convert_save` no longer checks for the old name.
- build.py / gen.sh run fetch_mod.py when `mod/nsmbw-8p.arc` is missing.
  Leftovers the user can delete: `mod/mkwcat-nsmbw.{zip,arc}`, `game/mod/mkwcat-nsmbw.arc`.
Tested: boots with the new archive, `Project: nsmbw-pc`, save file loads; full setup flow.
Not tested: a save being written (same file calls as before, only the names changed).

### Small ones
- **Esc no longer quits** (`rt_plat_win.c`); it still leaves fullscreen.
- **Title players dash** while running off for the menu and back in: `hle_StartAnimOrig`
  (dPyMdlBase_c::setAnm, 0x800D5DD0) and `title_anim()` in rt_menu.c - the dash animation (id 3,
  "B_dash") is started when the run starts, and the game's per-frame request for the walk
  (id 2) is dropped while running; back at their place the walk comes back on its own.
  (Their speed is fixed by the title demo, so dash input does nothing - tried.)
  `RT_ANMTRACE=1` logs their animation calls; `RT_DASHANM=4` tries the full-speed dash.
- **Title players all Super Mushroom** like the original (the module rolled a random power-up
  for the title): `hle_mod_fn_2_1B2680` in rt_title.c sets all 8 player modes to 1 after the
  module's `dScCrsin_c::executeState_resWaitProc2`. This is a wrapper around the module's code,
  not a port - `native_port_status.py` now lists such functions as *wrapped* instead of native.

---

## 2026-10-13 (late night) - Claude (instance 2): one build script that works on Windows

The user ran `build_windows.sh` on Windows and nothing happened (a .sh needs a Unix shell,
python3 and zig; double-clicked it just flashes or opens an editor). New:
- `build.py` - the whole build in Python (stdlib only): regenerate `out/` when inputs change
  (same inputs as `tools/gen.sh`, own stamp `out/.gen_stamp_py`), compile changed
  `out/recomp_*.c` and `runtime/*.c` in parallel with `python -m ziglang cc` (jobs = cores,
  limited to RAM/1.6 GB - the biggest file needs ~1.3 GB), link, deploy to `../game` exactly
  like the old script (exe written in place; `nsmbw_new.exe` if the game is running).
  Runtime objects also rebuild when a header (`runtime/*.h`, `*.inc`, `out/recomp_funcs.h`) is newer.
  `NSMBW_GAME_DIR` overrides the deploy folder (testing).
- `build_windows.bat` - double-click: finds Python (py launcher or python), installs `ziglang`
  with pip if missing, runs build.py, waits for a key.
- `build_windows.sh` now just runs `python3 build.py`. The runtime file list lives in build.py
  (RUNTIME) - add new runtime files there.
Tested on Linux: full regenerate + compile + link + deploy into an empty folder; a second run
does nothing but relink. The .bat itself could not be run here (no Windows).

---

## 2026-10-13 (night) - Claude (instance 2): no module name/version on the title screen

The module's `dEventOpeningTitle_c::create` wrote "<project> <version>" from its
`/Info/Version.json` ("<module name> v0.6.5") into the title layout's E3 version text box.
Both module functions of that class are now native in `runtime/rt_title.c` (new; added to
both build scripts) and the text stays hidden, as in the original game. The other one,
`executeState_PressButtonExitAnimeEndWait`, is unchanged in behaviour (title -> game setup).

`gcall` from rt_gamesetup.c is now shared as `rt_gcall(ctx, base, fn, nargs, ...)` (rt.h).
Tested: GL screenshots of the title (logo, Press 2, menu) show no version line; title ->
Game Select -> File Select still works.

Still showed the module's name (not on screen in normal play): the project name in the log
line `Project: ...` and in the module's crash screen (both read Version.json) - since renamed
to `nsmbw-pc`, see the entry above.

---

## 2026-10-13 (evening) - Claude (instance 2): 8-player code built in (phase 1-2), new setup menus

The user's direction: the 8-player mod is **not a separate mod** - it is part of this
codebase. Agreed approach: it is built in now, and moved to native C incrementally, one
subsystem at a time, using the recompiled module code as the reference. First subsystem: the
game setup scene, because the new menus need it. Progress table: `NATIVE_PORT.md`
(regenerate with `python3 tools/native_port_status.py`; source names come from
`tools/mod_source_names.py` -> `linked/mod_source_names.json`).

### Phase 1 - no more module branding in user-facing names
- **settings.ini keys** lost the `mod_` prefix and got plain names (`unlock_all_worlds`,
  `complete_all_levels`, `no_powerup_pause`, `infinite_lives`, `death_messages`, `fall_damage`,
  `skip_opening_movie`, `always_exit_course`, `shake_with_button`, `hide_motion_guides`,
  `yoshi_colors`, `liftable_tiles`, `bubble_swarm`, `deadly_1up`, `all_paths_open`).
  `rt_core.c`: `setting_aliases` + `settings_migrate_file()` rewrite an old settings.ini once
  (old keys still read through `rt_setting`'s alias fallback). `rt_mod.c` `mod_flags` uses them.
- **Save file**: the module's own save name (+ `-old`, `-tmp`, and a short variant) is now
  `nsmbw-save.json` (+ `nsmbw-save-old.json`, ...). (Done with a name alias in `rt_ios.c` and a
  one-time rename at that point; both replaced since by renaming the strings at link time - see
  the entry above.) Tested: an old save + settings.ini were migrated and loaded.

### Phase 2 - game setup scene native (`runtime/rt_gamesetup.c`, new)
All 9 module functions of `dScGameSetup_c` plus `dNumberOfPeopleChange_c::executeState_
NowEntrantRecruit` / `executeState_ButtonDecision`, and the game's FadeOut / FadeIn /
ConnectionCheck execute functions, are hand-written C (names in `hle_funcs.txt`, originals kept
as `orig_*`). Guest calls go through `gcall()`; state machines through the manager vtable
(+0x18 changeState, +0x28 getStateID; `state_is()` compares the state's name). Module data that
moved (8-player arrays) is addressed by `MOD_*` defines at the top of the file.

New flow (user spec), both modes:
`Title menu -> Game Select -> File Select -> character select -> game`, Back (1) the reverse
(`... -> Game Select -> title`).
- **Game Select** = the file select screen with one card (the middle one, cursor forced to 1),
  contents hidden, "NEW!!" text replaced by "NSMBW" (0.78 font scale), title "Select a Game",
  Copy/Erase and versus buttons hidden, d-pad/+/- blocked. Choosing it goes to File Select.
  **Pinned** (user): later this selects level/map packs; needs restructuring first.
- **File Select**: everything moved up 16 px (singleplayer: -70 + 16, as there are no versus
  buttons); the layout's own "Back 1" (`N_back`, was alpha 0) is shown. Back -> Game Select.
- **"Select Players" is gone**: the character select opens directly with 1 player
  (Singleplayer) or 8 (Multiplayer). `rt_menu.c`'s old workaround (auto-answer `hle_fn_2_3F000`,
  black overlay, `isAnimOn`/music hooks, the 10 s fix from the previous entry) is removed.
- **Character select**: title "Singleplayer"/"Multiplayer", the four remote icons hidden, "Add
  Wii Remote +" reads "Settings +" and opens the Controls screen (rt_menu overlay; any of
  1/B/2/A/+ closes it; still the placeholder - it will become per-player controls). 1 with
  player 1 undecided returns to File Select (before, it went back to the hidden Select Players
  screen, which answered itself again - so "1 did nothing").
- **Multiplayer**: Start Game appears once players 1 and 2 have both chosen and nobody else who
  joined is still choosing (a base still in `ConnectWait` = not joined). Joining by more players
  needs multi-controller input, which isn't there yet - the flow is ready for it.
- **Transitions**: every screen change fades out with the UI in place, the next screen is set up
  while black (file select: until its state is SelectWait, ~30 frames; character select: until
  `mCancelAllowed` and no base busy, ~35 frames), then fades in already in place. Start Game and
  Back on the character select also fade first; their exit animations play under black.
  Game input is dropped while any of this runs (`gs_filter_buttons`).

Things that were needed to make the re-open work (worth knowing for later screens):
- `dFileSelect_c` only runs while +0x2D9 is set; its GameStartWait clears it when the choice flag
  goes 0. So it is put into `StartWait` (0x80991F58) directly, then the scene's FileSelect init
  switches it on.
- The file cards (`dDateFile_c`) act on requests (+0xDD8; 5 = reset) only while active (+0xE49),
  but StartWait deactivates them first - they would never come back. `file_select_open()` does
  the reset itself (state InitialSetup 0x80991880, inactive, request 0).
- Address slip found while testing: `dRemoconMng_c::m_instance` is **0x8042A028** (r13/abs
  -24536 from 0x80430000), not 0x80429F28.

Tested headless (`RT_AUTOPRESS_FRAMES=1` - new: autopress times count game frames, so slow GL
runs behave like null-renderer runs): singleplayer title -> Game -> File -> character -> Start
Game -> world map -> level; every Back path (File->Game, character->File, File->Game->title);
multiplayer (8 bases, Settings + opens/closes Controls, P1 alone gets no Start Game, 1 undoes P1,
1 again -> File Select). Screenshots (GL, llvmpipe) checked for each screen and fade.

Open:
- Game Select is a reskin of the file select; real game/pack selection is pinned.
- Controls screen is still the placeholder (keyboard list).
- Free-for-All / Coin Battle (multiplayer file select) go to the 8-player character select;
  not tested beyond that.
- Next native ports: the rest of `dNumberOfPeopleChange_c` / `dCharacterChangeSelectBase_c`.

---

## 2026-10-13 (later) - Claude (instance 2): singleplayer start no longer waits ~10 s

**Symptom.** After picking a file in singleplayer, the screen stayed black for 10-20 s before
the character select appeared (input ignored meanwhile).

**Cause** (`rt_menu.c`, not the hang fix - the previous build's log shows the same wait).
Singleplayer skips the "Select Players" screen: `hle_fn_2_3F000` answers it with 1 player while
`menu_draw` keeps the screen black and `menu_filter_buttons` swallows input "until done". But
nothing ever marked it done except the 600-frame safety timeout, so every singleplayer start
waited 10 s (plus however long you took to notice).

**Fix.** `hle_fn_2_3F000` records `sp_answered_frame`; the menu filter ends the hold as soon as
the hidden layout (kind 4, tracked in `sp_kind4_frame`) is no longer drawn, or 30 frames after
the answer at most. The 600-frame timeout stays as a fallback and now logs
`singleplayer: gave up waiting for Select Players`; the normal path logs
`singleplayer: character select after N frames`. Headless test (RT_AUTOPRESS driving title ->
singleplayer -> file -> character -> map): N = 38 frames (~0.6 s), and the presses after it
reached the character select and the world map.

---

## 2026-10-13 - Claude (instance 2): random freeze fixed, GPU selection, hitching

Files touched (runtime only - the generated C in `out/` and the recompiler are unchanged, so no
regeneration is needed; `out/objwin` objects for `recomp_*.c` stay valid):
`runtime/rt_core.c`, `rt_hw.c`, `rt_plat_win.c`, `rt_plat_linux.c`, `rt_gxrender.c`, `rt_gxgl.inc`.
A new `game/nsmbw.exe` with all of this was built (zig 0.17.0, same flags as `build_windows.sh`).

### 1. The "random hang" (game frozen, log only shows `[audio] 32000 samples/s ...`)

**Symptom.** The picture stops, the window still responds, the audio DMA keeps running (so the
`[audio]` line keeps appearing every 10 s), `[gx] frame N` lines stop. In the user's log it hit
while the world map was being created after a level (`Create: LETTER_WINDOW`, then nothing).

**Reproduced** headless on Linux (see "Test harness" below) within ~90 s at the first title
stage load. Guest thread dump at the hang:

```
thread 80359938 (main, prio 16)   OSSleepThread <- mDvd_command_c::waitDone <- mDvd_toMainRam_c::create <- dDvd::loader_c::request ...
thread 80C7F460 (mDvd, prio 15)   OSSleepThread <- fn_8016B770 (the disc command thread's loop)
every other thread                OSSleepThread / OSReceiveMessage / OSWaitCond
```
Nothing runnable, nobody left to wake the main thread: a lost wake-up.

**Root cause.** Interrupts are only delivered at poll points: every 256th loop back-edge
(`PPC_POLL` in the generated code) and our `OSEnableInterrupts`/`OSRestoreInterrupts` HLEs.
The game has two classic check-then-sleep loops that are only "almost" race free on hardware:

```c
// mDvd_command_c::waitDone (8016B4F0)          // mDvd command thread (8016B770)
while (!cmd->done)                              for (;;) {
    OSSleepThread(&mgr->doneQueue);                 OSSleepThread(&mgr->cmdQueue);
                                                    while ((cmd = mgr->first)) { remove(cmd); run(cmd); }
                                                }
```
The compiler rotated both loops so the backward branch - i.e. our poll point - sits exactly
between the check and the `OSSleepThread`. If the poll delivers an interrupt whose handler wakes
a higher-priority thread, the OS switches right there (`__OSDispatchInterrupt` ends with
`__OSReschedule`). Example for waitDone: the IPC reply for the disc read wakes the mDvd thread
(prio 15 > main's 16), which finishes the command, sets `done`, wakes the done-queue (empty - main
isn't on it yet) and goes back to sleep; main resumes after its check and sleeps forever. On a
Wii the window is a few instructions out of millions; here every interrupt that became pending
since the last poll is delivered at that one spot, so it fires every few minutes of play.
Measured with `RT_PREEMPTLOG=1`: ~18 thread switches per second happened at poll points.

**Fix (rt_core.c, "deferred preemption").** Interrupt handlers still run immediately (flags,
callbacks, wake-ups), but `take_exception` raises the guest's scheduler lock (`Reschedule`,
the counter `OSDisableScheduler` uses; r13-20352) around the handler, so the `__OSReschedule`
at its end does nothing and only `RunQueueHint` (r13-20348) stays set. The woken thread then
gets the CPU at the running thread's next own scheduling call (OSSleepThread, OSWakeupThread,
mutex, yield ...) - for the races above that is the sleep itself, which is safe. If the thread
keeps computing without making one, `maybe_force_resched` calls the guest's `__OSReschedule`
(801B50D0) from a poll point at most 1 ms after the deferral, exactly like the end of an
interrupt handler would. With the fix, thread switches at poll points went from ~18/s to 0, the
game behaves the same otherwise (scene changes, audio rate 32000/s).
`RT_NODEFER=1` turns it off for A/B tests.

If a freeze still happens, the new watchdog (below) writes a `[hang]` block to the log - send
that log; it shows what every guest thread waits on.

Considered but not done: moving `PPC_POLL` in `tools/recomp.py` to the start of the straight-
line block that ends in the back-edge. It doesn't help when the check is inside a callee (the
mDvd thread's `first` is read by `fn_8016B5B0`), and needs a full regenerate + recompile.

### 2. Hang watchdog + better hang reports

* `rt_plat_win.c`: a watchdog thread logs once per hang when no frame was presented for 8 s:
  `[hang] no new frame for 8 s ...` + every guest thread with a backtrace + CPU state, and
  `[hang] frames again` if it recovers. Read-only, the game keeps running. `NSMBW_HANGWATCH=0`
  disables it.
* `rt_core.c rt_hang_state()` (printed by `rt_dump_trace`, also on crashes and by Linux
  `RT_HANGDUMP`): current fiber, poll-frame depth, host address of the last poll point
  (`image+0x...` on Windows), guest msr/lr/r1/r3, RunQueueBits/Reschedule/RunQueueHint,
  PI interrupt status/mask, decrementer, deferred/forced preemption counts, and the mDvd command
  queue (`first`/`last`; non-zero `first` while everyone sleeps = lost command wake-up, zero =
  lost "done" wake-up). It never reads MMIO addresses (it may run on the watchdog thread).

### 3. "Doesn't use the GPU on certain PCs"

`rt_plat_win.c`:
* Exports `NvOptimusEnablement = 1` and `AmdPowerXpressRequestHighPerformance = 1` from the exe.
  Laptops with integrated + dedicated GPUs otherwise run the game on the integrated one. This is
  the most likely cause of the report. (Verified in the PE export table of the new exe.)
* Pixel format: if `ChoosePixelFormat` returns Microsoft's software format (`PFD_GENERIC_FORMAT`
  without `PFD_GENERIC_ACCELERATED`) we pick an accelerated one ourselves.
* Context: tries 3.3 core, then 3.3 compatibility, then 4.0 before giving up on
  `wglCreateContextAttribsARB` (some drivers refuse the first).
* Logs `[gl] driver: vendor | renderer | version`, and refuses "GDI Generic" / GL < 3.3 with a
  clear log line instead of failing later.
* When the software renderer is used, the window title says
  `SOFTWARE RENDERER (no OpenGL 3.3 GPU driver found - see nsmbw_log.txt)`.

Crash fix found on the way (`rt_gxgl.inc`): `ui_register_texture` called GL with no GL context
when the software renderer is active -> null function pointer crash as soon as the title screen
loaded the game font. It now returns 0 there (menus just don't draw in software mode).

### 4. Hitching / CPU load

What the user's profile + log say (RTX 4080 PC): 63 % of the game thread's time was the guest
idle loop (SelectThread spinning while it waits for the next VI/audio interrupt), 13 % reading
the clock (rdtsc in the polls), ~10 % our renderer (vertex decode/transform), 7 % GL driver,
3 % recompiled game code. Steady state is 60 fps; hitches are single long frames at scene loads
(33-150 ms of guest work, "draws 0.0") and first-time shader compiles (up to 70 ms).

Changes:
* **Sampling profiler is now off by default** (`NSMBW_PROFILE=1` turns it on). It suspended the
  game thread 1000x per second (SuspendThread/GetThreadContext); every time the sampler thread got
  descheduled while the game thread was suspended, the game stalled for a whole Windows time
  slice - a hitch generator on machines with few cores.
* **Idle sleep** (`rt_core.c idle_wait`, `rt_hw.c hw_next_event_ns`, `plat_idle_wait`): when no
  guest thread can run (current thread = NULL) and no interrupt is pending, the host thread sleeps
  until ~0.4 ms before the next timed event (VI field, audio DMA block, delayed DSP mail,
  decrementer), max 4 ms, using a high-resolution waitable timer on Windows 10+. Frees the core
  for the GL driver threads, less heat / turbo loss on laptops. `NSMBW_IDLESLEEP=0` restores
  spinning. The `[perf]` line is followed by `(of which the CPU slept X ms per frame ...)`.
* Indirect calls (`ppc_call_indirect`, every C++ virtual call) hit a 16K-entry direct-mapped
  cache before the binary search over ~40,000 functions.
* `rt_tb_now` (every poll, every OSGetTick) uses one double multiply instead of two 64-bit
  divisions.

Not done yet (ideas, roughly by value):
* Shader compile hitches: compile asynchronously with `GL_KHR_parallel_shader_compile` and skip
  (or ubershader-draw) until ready; the binary cache already removes them from the 2nd session on.
* Vertex path: `decode_vertex` re-derives the format per vertex and memsets a 180-byte struct;
  a per-draw decoder plan would roughly halve "vertex decode/transform" (5 ms/frame in the
  8-player title scene).
* Frame pacing: presentation is unsynced (swap interval 0) at the emulated 59.94 Hz, so on
  60/144 Hz monitors frames are shown unevenly; an optional vsync / present-on-host-refresh mode
  would look smoother.

### Test harness (Linux, headless) used for all of the above

```
# build (any recent clang; objects can be cached between runs)
clang -std=gnu11 -O1 -w -Iruntime -Iout -DPPC_TRACE -c out/recomp_NNN.c ...   # all 104
clang -std=gnu11 -O1 -g -w -Iruntime -Iout -DPPC_TRACE -c runtime/rt_*.c (not rt_plat_win)
clang -no-pie *.o -lm -lpthread -ldl -o nsmbw_linux
# run from a game-style folder (orig/, nand/, mod/, settings.ini, disc/)
NSMBW_RENDERER=null RT_HANGDUMP=8 ./nsmbw_linux <gamedir> <gamedir>/disc
```
New test knobs (all runtime, no rebuild):
* `NSMBW_RENDERER=null` - nothing is drawn, everything else runs at full speed (game logic,
  audio, EFB copy bookkeeping, frame counting). Lets a 2-core box run the game at 60 fps.
* `RT_POLLSTRESS=1` - every loop back-edge polls (instead of every 256th): amplifies timing races.
* `RT_PREEMPTLOG=1` - logs interrupts that switched threads at a poll point (flags the two mDvd
  race windows explicitly). With the fix this prints nothing.
* `RT_NODEFER=1` - old (racy) preemption behaviour, for comparison.
* `RT_HANGDUMP=sec` (existed) - now also prints the CPU/scheduler state block.

Note for the next session: the headless llvmpipe GL path works too (`render_scale = 1` in
settings.ini), but is ~10 fps on 2 cores, which changes timing a lot.
