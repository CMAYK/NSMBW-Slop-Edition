# Online play

## Phase 1 - the connection (done, 2026-10-15)

`runtime/rt_net.c`. UDP, direct IP: the host forwards a UDP port (settings.ini `net_port`,
default 41790) on its router; others join with the host's public `IP:port`.

- **Title > Online**: Host a game, **Host port**, Choose the game and save file, Join (type
  the address with the PC keyboard: 2 or Enter starts typing, Enter joins, Esc stops),
  **Join port**, Leave; shows the status and who's in (player slot, name, ping).
  - Ports: Left / Right changes one by one, 2 types it (digits, Enter keeps it, Esc cancels).
    The host port is settings.ini `net_port` (can't change while hosting); the join port is the
    host's port, kept in online.ini. An address typed as `ip:port` moves the port to its row.
    For an ISP that blocks a port: pick another (1024-65535) on both sides.
  - `online.ini` (next to settings.ini): `name = ...` (what the others see), `join = ...` (the
    last address), `join_port = ...`.
- **NAT loopback / a second network**: HELLO carries a random nonce that WELCOME echoes. A
  joiner takes a WELCOME with its nonce from any address and talks to the host there from then
  on. (Joining your own public IP from inside your network, the host's router can hand the
  host's answer back from its LAN address; it used to be dropped: the host listed the player
  for 5 s, the joiner said "No answer from the host".) Packets from other addresses are logged
  once ("ignored a packet ... from ...").
- A joining PC sends HELLO with how many local players it has (its keyboard + its connected
  remotes); the host gives them free player slots (`rt_remotes.c` slot kind *online*) and
  answers WELCOME. Up to 7 joining PCs, up to 4 players each, 8 slots in all.
- Every frame the client sends INPUT: the last 8 frames of buttons for each of its players
  (redundant: a lost packet costs nothing). The host plays them in order, one per frame, 2
  frames behind the newest it has (jitter buffer); a frame that hasn't come yet repeats the last
  buttons; more than 12 behind, it catches up. The host sends STATE every 2 frames (its frame,
  the client's newest input it used - for reconciliation later - and the lobby). PING/PONG once
  a second. 5 s without packets = gone (its slots are freed). BYE on leaving / closing.
- On the host, online players are ordinary players: they join the character select, play, and
  their lights / channels work like local ones (online slots 1-4 drive emulated Wii Remotes,
  5-8 GameCube ports, like the keyboard).

What a client sees in phase 1: its own game, not the host's (that's phase 2 on).

## Testing (Linux, headless)

`RT_NET_HOST=port` hosts at start, `RT_NET_JOIN=ip:port` joins at start,
`RT_NET_FAKE_INPUT="frame:buttons hex:frames,..."` presses buttons on a client's player 1,
`RT_BTN_TRACE=1` (host) logs each slot's buttons. `RT_MENU_ONLINE=1` opens the Online screen
at the title; `RT_TYPE="sec:text"` types on the keyboard (Linux; `|` = Enter). NAT loopback is
reproduced by joining `127.0.0.2:port` (the host answers from 127.0.0.1). Verified: two instances on 127.0.0.1 - the
client became player 2 (registered as CHAN_1), its presses arrived in order on the host, and
the host freed the slot 5 s after the client quit.

## Deterministic mode (done, 2026-10-15)

`runtime/rt_det.c` + `rt_core.c` / `rt_hw.c`. With `g_det` the game no longer looks at the PC's
clock: a virtual clock (`g_vt_ns`) advances by fixed amounts (2000 ns per CPU poll, 20 per time
base read, 50 per hardware register read; idle jumps to the next event) and drives video
fields, audio DMA, DSP and the RTC; `det_pace` only throttles it to real time. Inputs are
latched once per video field (`det_field`). `det_state_hash` = XXH3 of MEM1 + MEM2 without the
pages the renderer writes (EFB copies, including their stride). Verified: two runs with the
same inputs are identical (null renderer, ~4000 fields, one run with random host delays
`RT_DET_JITTER`); GL vs GL identical; null vs GL drifts (TODO.md).

## Phase 2 - the online game: lockstep, started in place (2026-10-15)

Everyone runs the same game in deterministic mode; only buttons travel. The game starts from
the host's running game (a save state, docs/SAVE_STATES.md) - nobody restarts. (The first
version restarted every copy into the game from boot; the user didn't want the restart.)

**Menus** (rt_menu.c, 2026-10-15): press 2 > Singleplayer / Multiplayer / Options;
Multiplayer > **Local** / **Online**. Online opens the Online screen: Host a game / Choose the
game and save file (host) / Join (type the address) / Leave. Players join first and wait on
that screen. The host then picks **Choose the game and save file**: the game's own Game Select
and File Select (multiplayer) run on its PC; at the character select a panel over it lists
who's in and says **2: Start the online game** (1: back). Pressing it starts the online game
right there - everyone lands in the character select and picks their character with their own
controller. (The panel holds the other players' buttons until then.)

**Starting** (`rt_net.c`, protocol in the comment at "online game"): the host presses 2 on the
character select panel. The player map is the slots everyone already has in the host's game
(its own players' slots, the joined players' online slots).
1. OFFER: session id, input delay (see "Speed" below), clock,
   player map (the host's players first, then each PC's), the files the game uses (the host's
   save files, SYSCONF, setting.txt, its settings.ini). Each client fetches them (GETCHUNK /
   CHUNK, 320 requests in flight) into `<game>/online/p<n>/` and says READY.
2. The host switches to deterministic mode (`rt_det_enable`: the clocks go on without a jump)
   and at the next video field takes a save state of its game, S0 (`net_snap_point`, from
   `det_field`). It is in the online game from then on (it runs D fields ahead, then waits).
3. SNAP: the state's region table. A client freezes its game, switches to deterministic mode,
   takes a save state of its own game (to come back to), fetches the hash of every 4 KB page of
   S0 and compares them with its own memory at the same addresses. Both run the same game, so
   almost everything is the same: in the tests a client needed 256-263 of 30,038 pages
   (0.2 MB; guest stacks are always sent). WANT (bitmap) -> PAGES (blob size + hash) -> it
   fetches the blob (each page LZ-compressed, zero pages 2 bytes), puts S0 together, checks
   every page against the hash table and loads it: it continues exactly where the host took
   S0, in the online game.

**In the game**: rollback (the default since 2026-10-16, next section). The old way, lockstep
with an input delay, is the fallback (settings.ini `net_rollback = 0`, or a PC that can't keep
rollback states): at field F each PC sends its buttons for F + delay; a PC runs field F once
it has everyone's buttons for F. SHASH every 120 fields.

**Speed** (user, 2026-10-16: 2 players at 150 ms ping ran at ~20 FPS). In the first version a
client could only run a field once the host had *published* it, after collecting everyone's
buttons, so a client's own buttons made a full round trip first. With a delay of 3 fields
(50 ms) against a 150 ms round trip the game ran 3 fields per round trip, 20 FPS. Two changes:
- **Direct path**:
  - The host sends its own buttons to every client each field (SPEER), and passes on each
    client's buttons to the others as soon as it has them.
  - A client runs field F as soon as it has everyone's buttons for F, so only one trip has to fit
    in the delay: host <-> client is half a round trip, client <-> client (through the host) half
    of each one's. The host's published field is used when it comes first.
  - Every field a client ran directly is checked against the host's published one (SFRAME).
  - SPEER also tells each client from which field the host uses its buttons. While it is away it
    runs only published fields.
- **Automatic input delay**: settings.ini `net_input_delay = 0` (the new default; the old
  default 3 is rewritten to 0) picks it when the game starts. It takes the worst of each
  client's last pings, then half the worst trip + 8 ms, in fields, + 1. That gives 6 fields
  (100 ms) at 150 ms ping and 2-3 nearby. 1-15 fixes it. The window title shows it ("Online -
  input delay 100 ms").
- **Getting the game again (RESYNC)**:
  - When a client finds that a field it ran differs from the host's, it asks the host for the
    game again. That happens when the host played on without it: its packets were lost but it
    kept getting the host's. It also asks when the 120-field state check fails, which used to
    mean OUT OF SYNC for good.
  - The client freezes. The host takes a state at the next field and sends the pages that
    differ (0.1-0.4 MB in the tests). The client loads it and catches up.
  - An epoch in SINPUT keeps buttons from before the reload out. More than 3 in a minute means
    OUT OF SYNC.
- Catching up asks for the next fields before running out (SFREQ), and never stores fields
  more than 496 ahead (they used to overwrite the ones still to come after a long absence).

Measured (Linux, two copies, `RT_NET_LAG=75` each = 150 ms round trip):
- old way, delay 3: 22.9 FPS, 15 s of waiting per 600 fields;
- new: delay 6 chosen automatically, 59.9 FPS, 0-10 ms of waiting per 600 fields, every field
  on the direct path, state hashes identical;
- with jitter (60 ms ± 30): delay 7, 59.9 FPS, identical;
- 3 PCs: identical;
- a forced desync: recovered in 0.1 MB, identical afterwards;
- one-way packet loss for 2.5 s: the client found "field 2013 differs", got the game again,
  identical afterwards;
- quit + rejoin: identical, and back on the direct path.

## Rollback (2026-10-16, the default)

User, 2026-10-16: "the goal was 0 ms input latency, 250 ms is unplayable", and players who
couldn't catch up with the host watched the game like a video until they were dropped. The
lockstep game above can't do 0 ms: a field needs everyone's buttons, so the delay has to
cover the trip (and the automatic delay took the worst of 6 pings: one 568 ms spike made it 250 ms).

**How it plays.** Every PC runs every field at once with its own buttons. Nobody waits. For
the other players it uses their last known buttons. When their real buttons arrive and differ
from that guess, the game goes back to that field and runs the fields since then again,
silently, in the same frame (a *rollback*), and carries on. Your own player answers at once
(input delay 0). A rollback can make another player jump a little. At 150 ms ping the
test games went back 5 fields (80 ms) on average, about 45 times every 10 seconds, with both
players pressing buttons all the time.

**States** (`rt_state.c`, `rb_*`): the state of every field, kept in memory for the last 48
fields (0.8 s). A whole save state is 117 MB, so a field's state is only what changed:
- MEM1, MEM2 and ARAM are *watched*: the PC tells us which 4 KB pages were written since the
  last field. Windows: `GetWriteWatch` on memory made with `MEM_WRITE_WATCH`. Linux (tests):
  the pages are made read-only and the first write to each one is caught (SIGSEGV + mprotect).
- The locked cache and the runtime's own state sections (5.3 MB) are compared with a copy.
- The guest stacks (fibers) are copied, from each one's stack pointer up (~11 KB).
- Each written page's old contents go into that field's *undo list*, and a copy is kept up to
  date. 130-180 pages (~0.6 MB) per field; ~2 ms per field on the test VM (Linux; Windows has
  no faults to catch).
- Going back to field R: from the newest state back to R, every undo list is applied, newest
  first. Then the stacks are put back, and the host fiber jumps into `rb_save` of field R,
  which returns 1. The game is then exactly where it was at the start of R.
- Fields whose buttons are already final (the host's word: on the host, everyone's buttons are
  in; on a client, the host has published it) keep no state: nothing can go back to them. What
  they change goes into the newest kept field's undo list, which stays exact. Catching up runs
  only such fields, so it skips the cost.
- A file read into guest memory goes through a buffer (`rt_fread_guest`). On Linux the kernel
  can't write a read-only page; on Windows a write by the kernel might not be counted.

**Running fields again** (`g_resim`): nothing is drawn (draws, EFB copies; an XFB copy still counts the
frame), no sound is mixed or played, and the pacing is off. Only the game runs.

**The buttons** (`rt_net.c`, `rb_field`, every video field):
- Each PC sends its own buttons for the field to the host (SINPUT), everything the host hasn't
  confirmed yet. That is up to 4 packets of 32 fields, so a lost packet costs nothing.
- The host forwards each PC's buttons to the others at once (SPEER). The packet also says the
  host's field (for time sync) and how far the host has the receiver's own buttons (the ack).
- The host *publishes* each field whose buttons are all in (SFRAME). These are final, and a
  client catching up asks for them (SFREQ).
- When a PC's buttons stop coming, the host waits 1.5 s. Then that PC is *away*: the host
  decides "no buttons" for it from where they stopped and tells everyone, that PC included. When
  that PC's buttons come in again for fields close to now, it is back: from the field after the
  last one decided for it. Its own buttons for the decided fields are replaced by the host's
  word (it goes back if it can, otherwise it gets the game again: RESYNC, as in lockstep).
- A wrong guess is noticed when the real buttons arrive (`rb_store`): the field already ran
  with something else, so `SS.mis` is the earliest such field. That includes the field running
  right now: buttons can come in after it decided, from the network poll in the middle of the
  field (the first desync found, 2026-10-16).
- Nobody runs more than 20 fields (330 ms) ahead of a PC's last known buttons. Past that it
  waits, which happens only if that PC's buttons don't arrive. A client that is waiting asks
  for the published fields from its gap, in case packets were lost.
- **Time sync**: each PC estimates where the others are (their last field + the time since +
  half the ping). If it is 2 or more fields ahead of someone, it runs 2-4 ms later per field,
  so the rollbacks are shared fairly. The pacing's clock moves (`det_pace_slow`). A plain
  sleep was made up for by the pacing straight after, so the first version never slowed down.
- **Catching up** (after loading the host's game, a RESYNC or a drop): a client more than
  8 fields behind the host runs unpaced and silently, like a rollback, without states. Then
  it logs `caught up: N fields in X s (Y fields/s; the game runs 60)`. On the test VM:
  348 fields in 1.8 s (194 fields/s) with a second copy running. A PC that can't keep up
  holds the others back through the time sync and the 20-field limit; nobody plays on
  without it unless it stops answering.
- **Starting**: the host waits for everyone to load the game (up to 60 s, "Online: waiting
  for everyone to load the game"), so nobody starts behind.
- The state check (every 120 fields) is computed when that field runs, and sent only when every
  button before it is final and no rollback is due. It is recomputed when that field runs again.

settings.ini: `net_rollback = 1` (0: lockstep), `net_input_delay = 0` (with rollback: 0-8 extra
fields of delay, fewer rollbacks; 0 = none).

Measured (Linux, 2.1 GHz VM, two or three copies on 2 cores, null renderer, RT_AUTOPRESS on
both):
- 150 ms round trip (`RT_NET_LAG=75` each): ~45 rollbacks per 600 fields, 5 fields back on
  average; state hashes identical (24 checks); no waiting.
- 300 ms round trip (`RT_NET_LAG=150`): ~55 rollbacks, 7 fields back; identical for 31 checks;
  no waiting.
- 3 PCs (40/60 ms): identical.
- A client's packets lost for 0.5 s: both waited briefly; identical.
- A client's packets lost for 3 s: the host played on without it, and it was back; identical.
- The network down both ways for 3 s: the host played on, the client caught up from the
  host's published fields and was back; identical.
- Quit + rejoin: the game at field 1728 (6 MB). The client caught up and was back after
  6 s; identical.
- The FPS of these runs (~45) is the VM: 2-3 copies share 2 cores. One copy needs ~8 ms per
  field there before rollback. Expect much more headroom on a desktop PC.
- Not run yet: Windows (GetWriteWatch), a real renderer.

**The game's files**: the game's NAND is
`<game>/online/<name>/nand` (`host` / `p<n>`; nobody's own save is touched); its game settings
are the host's except each PC's display / sound / network ones. The window title says
"Online (host)" / "Online" / "Online: waiting for the other players (N s)" / "Online: loading
the host's game (N%)" / "Online: OUT OF SYNC".

**Players dropping out** (user, 2026-10-15: one player closing the game ended it for everyone):
- a PC whose buttons don't come in time (1.5 s) is *away*: the host publishes the fields without
  it (its players press nothing) and the game goes on for everyone else ("Player isn't
  answering - playing on without them"; the host's title says "a player is away"). When it
  sends buttons again for fields near the ones being published it is back (the host waits a few
  fields for it to get ahead again). A client cut off from the host keeps trying for 30 s
  ("Online: reconnecting to the host (N s)"), then goes back to its own game ("lost the
  connection to the host"). Back after a drop, it runs unpaced through the fields it missed
  ("catching up"), asking the host for them (SFREQ; the host keeps every published field).
- a client that closes its game (BYE) is *gone*: its players wait, the others play on. It can
  join again: Multiplayer > Online > Join (same name, or any gone player's place). The host
  sends it the game's files as they are now, takes a state of the game at the next field and
  sends that (pages that differ only); it loads it, catches up and is back - nobody waits for it.
- only the host ending it (Leave, closing its game: "the host closed the online game") or the
  host being unreachable for 30 s ends the game for everyone.
After the game ends, a message over the game says why for a few seconds.

**Ending** (Title > Online > Leave the online game - the host's choice, on every PC; the host
closing its game; the host unreachable for 30 s):
everyone loads the save state of their own game from before and plays on from there (the host:
S0, its game at the start). The Online screen says why ("The online game ended: ..."). A client
leaves the lobby then (join again for the next game); the host keeps hosting. Deterministic
mode stays on (it plays normally).

Limits: a client can't open our menus in the game (only player 1 drives them) - it leaves by
closing the game; the game version must match (a state of another build is refused: "a
different version of the game") but the `mod/` folders aren't compared.

## Testing (two instances, Linux, headless)

Both copies can use the same game folder. Host: `RT_NET_HOST=41800 RT_NET_START=400` (starts
the online game once someone joined, at frame 400), client: `RT_NET_JOIN=127.0.0.1:41800`.
`RT_DET_HASH=file` writes each field's hash, `RT_NET_LOG_HASH=1` logs the 120-field ones,
`RT_NET_TEST_DESYNC=<field>` flips a byte on that PC (must give OUT OF SYNC),
`RT_NET_LAG=ms[:jitter]` holds every packet this copy sends that long (give both copies half the
round trip), `RT_NET_NO_DIRECT=1` (client) only runs the host's published fields (the old way),
`RT_NET_BLIP_OUT_AT=<field>:<ms>` loses only this copy's packets,
`RT_NET_LEAVE_AT=<field>` leaves the online game at that field, `RT_NET_QUIT_AT=<field>` closes
the game (BYE), `RT_NET_BLIP_AT=<field>:<ms>` drops the network for that long,
`RT_NET_LOG_USED=file` writes the buttons each field ran with (last line per field wins:
compare two PCs' files to find the first field that differs), `RT_RB_TEST=start:every:depth`
(no online game) goes back `depth` fields every `every` fields from `start` - with
`RT_DET_HASH` the hashes must not change, `RT_RB_DEBUG=1` logs the state times,
`RT_NET_ONLINE_SETUP=1` (host) opens the Online screen at the title once someone joined (with
RT_AUTOPRESS pressing 2 it goes through game / file select to the character select and starts).
Verified (2026-10-15):
- the menu flow: Online screen -> Game Select (multiplayer) -> File Select -> character select
  -> 2 on the panel -> the client loaded into the character select (3.3 MB) and picked Luigi
  with its own buttons while the host had Mario; identical for 1,783 fields;
- a 5 s network drop on the client: the host played on without it, the client reconnected,
  caught up and was back; the client then closing its game: the host played on; a new copy
  joining again got the game at field 4096 (12 MB), caught up, and was identical to the host
  for 2,390 fields (and the first client for 2,038 before it left);
- start in place at field 438: the client needed 256 pages (0.2 MB), loaded, and both games
  were identical for all 4,198 fields after (host pressing through the title into 1-1);
- the host leaving at field 1500: both went back to their own games and kept running.

## Next

Rollback: the save states are there (docs/SAVE_STATES.md); a state takes ~100-170 ms and
117 MB now, far too much for every frame - it needs only the pages written since the last
state (dirty-page tracking: write-protect + fault, or hashing) and states kept in memory.
