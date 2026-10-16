# Updates

The game can update itself from your server, for example a copyparty server or any web server
that serves files over HTTP(S).

**What the player sees.** Safety warning → "Hold the Wii Remote sideways" → **update screen** → title.
The update screen is the "Hold the Wii Remote sideways" screen's own layout: its striped panel
stays, and the picture and text change to the update's, in the game's font. See
docs/BOOT_SCREEN.md, "The update screen uses this layout too":

- **Checking for updates**, with a spinner. The check starts in the background when the game
  starts, so by the time the screen appears it has usually finished. The spinner still stays up
  briefly so the step is visible.
- **You have the latest version**, **Couldn't reach the update server**, or
  **Update check failed: why**. Each stays up for a moment, then the title starts.
- If there is an update, a **download bar** shows "12.3 of 61.9 MB - 4.2 MB/s". Pressing **1** (or B)
  skips the download, and files that finished downloading are kept for next time. When the
  download finishes, the screen shows **Installing...** and **Restarting...**, and the new
  version starts on its own.

The game version appears in the corner of the update screen and under the Options panel.

## One-time setup (the author)

1. **Make your signing key.**
   ```
   python tools/publish_update.py keygen
   ```
   - The secret half goes in `~/.nsmbw_update_key` (on Windows, `C:\Users\<you>\.nsmbw_update_key`).
     **Back it up and never share it.** Without it you can't publish updates that existing
     copies of the game accept. Anyone who has it can.
   - The public half goes in `recomp/update_key.pub` and is built into the game.
2. **Set the server address.** Put it in `recomp/update_url.txt` (one line). It is the folder
   address on your server, e.g. `https://files.example.com/nsmbw/`. A copyparty password can go
   in the address as `?pw=...`. Players then need no password, but the password is built into the game.
   A volume that anyone can read (`r` for everyone) is the better choice.
3. **Rebuild** (`python build.py`). The address and key are built in, and from then on every copy checks for updates.

### copyparty: one folder anyone can read

copyparty has no settings screen for permissions; they go in a config file (or on its command
line). A config file, e.g. `party.conf`, used with `copyparty.exe -c party.conf`:

```
[accounts]
  yourname: yourpassword

[/]                       # everything you already share: only you
  D:\shared
  accs:
    rwmd: yourname

[/nsmbw]                  # the update folder: everyone can read, you can write
  D:\nsmbw-updates
  accs:
    r: *
    rwmd: yourname
```

The update folder's address is then `http://<your public IP or domain>:3923/nsmbw/`. Forward
copyparty's port (TCP, 3923 by default) on the router. Delete rights (`d`) are needed to
replace `latest.txt`.

## Publishing an update

1. Run **`build_windows.bat`**. It first asks for the version number of this build, and shows
   what players have now when the server can be reached:
   ```
   Version of this build: 0.1.0   (players have 0.1.0)
     Enter = keep 0.1.0,  + = 0.1.1,  or type a version:
   ```
   Enter keeps it (an ordinary rebuild), `+` takes the next one, or type any version, e.g.
   `0.2.0`. Players only take a version higher than their own. The choice is saved in
   `recomp/VERSION`; `python build.py --version 0.2.0` sets it without asking.
2. After building, it compares the version with the server's `latest.txt`:
   - If the server already has this version (an ordinary rebuild), it prints one line and does nothing.
   - If this version is higher, it asks **"Publish version 0.2.0 as an update for players? [y/N]"**.
     Answer y, type a line about what changed, and it packs and uploads the build logged in as you.

   Other ways: `python build.py --publish --notes "..."` (no question), `publish_windows.bat`
   (publish the game folder as it is), or `python tools/publish_update.py publish`.
   - The build prints `version 0.2.0 - updates from <address>`, or `updates OFF in this build (why)`.
     A build with updates off checks nothing.
   - Publishing makes `recomp/out/update_site/` from the game folder: **`nsmbw-<version>.zip`**
     (the zipped build) and `latest.txt`. It uploads them to the address in `recomp/update_url.txt`,
     the zip first and then `latest.txt`, so players never see a list whose zip isn't there yet.
     Then it reads `latest.txt` back without a login, the way players see it.
   - The zip is also a complete download for new players: unzip it into a folder and add the disc.
   - Older `nsmbw-*.zip` files on the server can be deleted once nobody is still downloading them.

**Your copyparty login.**

- The first publish asks for your copyparty user name and password. It checks with copyparty
  that this login may write and delete in the update folder, then offers to save it.
- The login is saved in `C:\Users\<you>\.nsmbw_upload_login`, outside recomp and the game, so
  it is never published. The password is encrypted for your Windows account (DPAPI), so only you,
  on this PC, can read it.
- A saved login that stops working is forgotten, and you're asked again.
- `python tools/publish_update.py login` saves a new login; `... logout` forgets it.
- `--user` / `--password`, or `NSMBW_UPLOAD_USER` / `NSMBW_UPLOAD_PW`, override the saved login
  for one run.
- The login is sent as HTTP basic auth (`user:password`), which works with and without
  copyparty's `--usernames`.

**Who can delete.** With the config above, everyone else can only read. An upload without a
login is refused (HTTP 401, tested). Your account needs `d` in this one folder, because copyparty
treats replacing a file as deleting it and writing a new one. Without `d` the publish stops
with that message.

**Which files are published.** `recomp/update_files.txt` lists them, one pattern per line,
relative to the game folder; `!` leaves files out. The default is `nsmbw.exe`, `orig/`, `mod/`
and `ui/` without `mod/*.zip`. Player data and the disc are never published, whatever the list
says, and the game never writes them from an update:

- `disc/`, `nand/` (saves), `online/`, `update/`
- `settings.ini`, `wiimotes.ini`, `online.ini`, `profiles.json`, `update.ini`

## The server's files

```
latest.txt            nsmbw-update 2
                      version 0.2.0
                      notes what changed
                      zip <size> nsmbw-0.2.0.zip
                      file <sha256> <size> <offset> <packed size> <method> nsmbw.exe
                      file <sha256> <size> <offset> <packed size> <method> ui/controls_bg_43.png
                      ...
                      sig <Ed25519 signature of every byte above this line, hex>
nsmbw-0.2.0.zip       the zipped build (deflate, level 9)
```

`offset` is where the file's packed data starts in the zip; `method` is 8 (deflate) or 0 (stored).
The game fetches only the files whose hash differs from its own, each with one HTTP range
request for exactly that part of the zip. An update that only changes the exe costs about
15 MB, not the whole build.

## How the game does it (`runtime/rt_update.c`)

1. **Check.** At start, a background thread fetches `latest.txt`:
   - Windows uses WinHTTP with the system proxy; the Linux test build uses `curl`.
   - A list without a good signature from the built-in key is ignored, as is one that names a
     protected path, `..`, or an absolute path.
   - The game downloads only if the list's version is higher than its own. Then it hashes its own
     copy of each listed file, and only files whose hash differs are fetched.
2. **Download.** Each changed file's part of the zip is fetched with an HTTP range and unpacked with
   stb_image's deflate decoder. It is checked against the list's size and SHA-256, then kept as
   `update/files/<sha256>`. Files already there with the right hash are reused after a skip or a
   failed try. A server that ignores ranges gives "the server can't send part of a file".
3. **Install.**
   - The running `nsmbw.exe` can't be overwritten, but it can be renamed. It becomes `nsmbw.exe.old`
     and the new exe takes its name.
   - Every other file goes into `update/pending.txt`, and the game restarts (`plat_restart`).
   - At start, before anything opens those files, the new copy moves them into place
     (`update_install_pending`). Each old file is kept as `.old` until the new one is in place,
     so a failure puts it back.
   - `.old` files that can't be deleted yet (the old exe, until that process has exited) are
     listed in `update/cleanup.txt` and deleted at the next start.
4. **Hold the boot.** The boot scene asks for the title (`DoSceneChange(RESTART_CRSIN)`) every
   frame until the request goes through. `rt_lyt.c` holds that request while `update_boot_hold()`
   says so. The screen's timing lives in `update_boot_hold()`, so a headless run behaves the same
   as a windowed one. `rt_bootscreen.c` turns the boot's layout into the screen (text, what's
   hidden); `rt_menu.c` draws the spinner / bar / small text over it.

**Names.** The build-time macros are `GAME_VERSION`, `GAME_UPDATE_URL` and `GAME_UPDATE_KEY`.
`RT_VERSION` can't be used because windows.h defines it (a resource type, 16). It silently
replaced the version string, and the Windows build crashed at start in the first log line.

**Settings and overrides.**

- `settings.ini`: `update_check = 0` turns the check off, and the screen is skipped.
- `<game>/update.ini` (optional): `url = ...` uses another server. `key = <64 hex>` uses another
  public key, for a fork or for testing.
- No address or no key built in: the check is off.

**Online.** HELLO carries the game version. The host turns away a different version with
"Different game versions (host 0.2.0, you 0.1.0) - restart both games to update".

## Testing (Linux)

- `RT_UPDATE=1` checks even in deterministic runs (normally off there).
- `RT_UPDATE=0` never checks.
- `RT_UPDATE_THROTTLE=bytes_per_s` slows the download, to look at the bar.

A local copyparty works as the server:

```
python3 -m copyparty -p 3923 -a ed:secret -v site:nsmbw:r:rwmd,ed
HOME=/tmp/k python3 tools/publish_update.py keygen
python3 tools/publish_update.py publish --game <folder> --upload http://127.0.0.1:3923/nsmbw/ --password secret
echo "url = http://127.0.0.1:3923/nsmbw/" > ../gU/update.ini
```

Verified (2026-10-15, Linux):

- download → restart → install at start → "latest" → title
- skip mid-download: the finished files are kept
- unreachable server
- tampered list: the signature fails
- signed list naming `settings.ini`: refused
- a damaged file on the server: the hash fails
- a mismatched online version: refused
- the Ed25519 check against RFC 8032 vectors and 119 Python-signed or tampered cases

The Windows side compiles but hasn't been run yet: WinHTTP, renaming the running exe, the restart.
