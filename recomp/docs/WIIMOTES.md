# Wii Remotes (up to 8 real ones)

## For the player

**Pair once:** Options > Controls (or "Settings +" on the character select) > **Pair Wii
Remotes**, then press the red **SYNC** button in the remote's battery compartment. The game pairs
it with the PC "for good": the remote stores the PC like it stores a Wii. Do this for each remote
(the pairing keeps looking until you choose **Stop Pairing** or leave with 1).

**After that:** just press any button on the remote - it connects to the PC by itself (also after
a reboot), and the game picks it up. No Windows Bluetooth settings needed. A remote takes the next
free player slot when it connects.

**Closing the game** switches the remotes off (their Bluetooth link is dropped; they stay
paired). Also after a crash.

- Use the **SYNC** button. A remote that can't be paired (or one in 1+2 mode) is still connected
  for the session - the dialog says so.
- A remote paired the old way in Windows' Bluetooth settings: press its SYNC button while the
  dialog is pairing and it is paired again properly.
- **Forget:** select a player's row and press 2: the game forgets that remote and Windows forgets
  the pairing.

**Player slots:** a remote that connects takes the **next free player slot** (the lowest one
without the keyboard or another remote); when it disconnects, the slot is free again. Positions
aren't remembered - only which remotes are paired (they reconnect with any button). In Controls,
select a player and press **Left/Right** to change its controller for this session: None,
Keyboard or one of the known remotes; taking one another player has swaps the two, and a remote
set to None sits out (no lights) until it disconnects or is picked again. The keyboard is always
some player's (player 1 at first; this one *is* remembered) - it can be moved, not removed, so
keyboard + 7 remotes works. While player 1's slot has nothing connected, the keyboard also drives
player 1 (the game's own menus only listen to player 1). `wiimotes.ini` (next to `settings.ini`):
`keyboard = N` and `known = <address>` per paired remote (older files with `slotN = ...` are
converted).

**Lights** show the player number:

| Player | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| Lights | `1000` | `0100` | `0010` | `0001` | `1100` | `0110` | `0011` | `1001` |

Players 5-8 light two neighbouring lights (1+2, 2+3, 3+4, then the two ends 4+1). The number is
the game's: it numbers players in the order their controllers come in, so with an empty slot
before it a remote shows the lower number the game gave it.

## How it works

| Piece | File |
|---|---|
| Real remotes on Windows: up to 8 HID devices (057E:0306 / 0330), each with a reader and a writer thread; its Bluetooth address from the HID serial number string | `runtime/rt_plat_win.c` (`plat_wm_*`) |
| Pairing (SYNC button; PIN = the PC's Bluetooth address, reversed): Windows Bluetooth API (`bthprops.cpl`, loaded at runtime); remembered remotes that are neither connected nor authenticated are removed; inquiry for "Nintendo RVL-CNT-01(-TR)"; a PIN without 0 bytes goes through `BluetoothAuthenticateDevice` (one WCHAR per byte, as Dolphin); a PIN **with** a 0 byte can't (the text ends there - error 1244; the user's PC address ends in :00), so `BluetoothAuthenticateDeviceEx` + a `BluetoothRegisterForAuthenticationEx` callback answering with the binary PIN, on its own thread with a 20 s limit; then `BluetoothEnumerateInstalledServices`; HID service switched on (also when pairing failed: the remote then works for the session) | `runtime/rt_plat_win.c` |
| Slots (keyboard / remote / none), memory, lights, each slot's buttons for the game (`remotes_game_buttons`), GameCube translation | `runtime/rt_remotes.c` |
| The game's 4 Wii Remotes (WPAD channels 0-3) on the emulated Bluetooth controller; real ones pass straight through | `runtime/rt_wiimote.c`, `runtime/rt_bt.c` |
| SYSCONF BT.DINF holds the 4 emulated remotes (registered + active, so remote i = channel i); patched at boot | `runtime/rt_ios.c`, `tools/make_nand.py` |
| GameCube controller ports (serial interface) for players 5-8 | `runtime/rt_si.c` |
| The dialog | `runtime/rt_menu.c` (`controls_draw` / `controls_input`) |
| Remotes off when the game closes: `IOCTL_BTH_DISCONNECT_DEVICE` (0x41000C) on the radio with each remote's address - the remote powers off when its link goes | `runtime/rt_plat_win.c` (`wm_shutdown`) |

**Why GameCube ports for 5-8:** the game (Revolution SDK WPAD) has 4 Wii Remote channels. The
8-player module's players come from its channels CHAN_0-3 (remotes), CHAN_GC_0-3 (GameCube
controllers) and CHAN_CL_0-3 (split Classic Controllers). Slots 5-8 are fed to CHAN_GC_0-3: the
remote's buttons become a GC controller's the way the module reads GC controllers (measured by
pressing each GC button: A -> 2, B -> 1, X -> A, Y -> 1+B, Start -> +, Z -> - and shake, GC
left/right/down/up -> remote up/down/left/right, i.e. the pad held sideways). Shake on those
remotes comes from their accelerometer (report mode 0x31, a sharp change -> Z for 12 polls).

**Player numbers:** the module numbers players in the order their controllers appear
("Register player #n: CHAN_x" in the log). So the game's remotes connect one at a time in slot
order (each after the one before has been in for 30 frames), and the GC ports appear after them,
in order: with all remotes there at start, slot n = player n. A remote that comes later gets the
next free player number from the module - its lights always show the number the module gave it
(read from `dRemoconMng_c`: player p's connection +12 = channel).

**SI note:** SITransfer holds a port's first transfer until "last transfer + delay"; the system
time is negative early on here (low memory 0x800030D8 = -1 high word), which put that a minute
ahead - the type probes waited ~70 s. `hle_SITransfer` treats a port that never transferred as
long ago.

## Testing (Linux, headless)

- `RT_FAKE_WM=n` - n pretend remotes (addresses AABBCC00000i) that answer like real ones;
  `RT_FAKE_WM_SCRIPT="remote:frame:buttons hex:frames,..."` presses their buttons,
  `RT_FAKE_WM_GONE="remote:frame,..."` takes one away, `RT_FAKE_WM_PAIRED="frame:remote,..."`
  pretends it was just paired. Their lights are logged (`[fakewm]`).
- `RT_BTN_TRACE=1` - logs each slot's buttons for the game when they change.
- `RT_GC_TEST="frame:GC buttons hex:frames,..."` - a controller in GC port 0.
- `RT_CONTROLS_AT=frame` - opens the Controls screen at the title.

Verified so far (headless, fake remotes): 8 remotes -> players 1-8 in order (CHAN_0-3, then
CHAN_GC_0-3), lights 1000 ... 1001; a remote leaving -> "Remocon Shutdown"; multiplayer
character select: players 2 (remote) and 5, 6 (GC ports) join, move and choose; the Controls
screen; slots: keyboard in slot 3 with remotes in 1, 2, 5, 6; keyboard in slot 2 (its presses reach slot 2
only, the title's "2" still reaches player 1), a paired remote taking the first free slot,
Left/Right cycling and swapping. **Not verified:** real Bluetooth pairing and real remotes on Windows (no hardware here).
