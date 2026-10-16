# Discord Rich Presence

While the game runs, your Discord profile shows what you're doing in it, for example:

> **Playing New Super Mario Bros. Wii**
> World 1-2
> Multiplayer (3 of 8) - 12:34 elapsed

What it shows (`runtime/rt_discord.c`, looked at twice a second, sent when it changes):

| where | first line | second line |
|---|---|---|
| title screen and its menus | On the title screen | - |
| file / character select | Getting ready | - |
| opening movie | Watching the opening | |
| world map | World N map | Singleplayer / Multiplayer / Online, N players |
| a level | World 1-2, World 3 - Tower, Airship, Toad House, ... | same |

The big picture is the application's `logo` asset; the small one is player 1's character
(asset named by the character id: `mario`, `luigi`, `ala_gold`, `buckenberry`, `toadette`,
`toadsworth`, `daisy`, `rosalina`, `goomba`, `retro_smb1`, `retro_smw`, `retro_sm64`; a missing
asset just shows nothing). Multiplayer shows the party size (players in / 8). The time counts
from the game's start.

## Setting it up (once, by the game's author)

Discord shows "Playing <name>" from a Discord *application*; the game needs its ID.

1. https://discord.com/developers/applications > **New Application**, name it what the profile
   should say after "Playing" (e.g. `New Super Mario Bros. Wii`).
2. Copy the **Application ID** (General Information) into `recomp/discord_app_id.txt` (just the
   number) and build. The build puts it in the exe (`out/rt_version.h`, `GAME_DISCORD_ID`);
   without the file, presence is off in that build and the log says so.
3. **Rich Presence > Art Assets**: upload `logo` (at least 512x512) and the character pictures
   under the names above. New assets can take a few minutes to show up.

Players don't need to do anything: it works whenever the Discord desktop app is running on the
same PC (Windows: `\\.\pipe\discord-ipc-N`; Linux: `$XDG_RUNTIME_DIR/discord-ipc-N`, also the
Flatpak / Snap paths). settings.ini `discord_presence = 0` turns it off (a local setting: not
taken from an online host).

## How it works

No Discord SDK. The game speaks Discord's local IPC itself: frames of `opcode (u32 LE),
length (u32 LE), JSON`. HANDSHAKE (op 0: `{"v":1,"client_id":"<id>"}`) once, then
`SET_ACTIVITY` commands (op 1) with our process id. A background thread owns the connection:
retries every 15 s while Discord isn't running, reconnects when it restarts, and sends at most
one update per 4 s (Discord allows about 5 per 20 s; the newest one always goes out). The game
thread only builds the text. Log lines: `[discord] ...`.

Testing without Discord: `tools/test_discord_ipc.py` is a fake Discord (Linux socket) that prints
what the game sends.
