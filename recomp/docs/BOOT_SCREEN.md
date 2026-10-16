# The boot's controller screens (keyboard players)

After the safety screen, the boot shows **"Hold the Wii Remote sideways."** with a picture of two
hands holding the remote. When player 1 plays on the keyboard (`remotes_slot_keyboard(0)`), that
screen comes first as it is. Pressing 2 doesn't go on to the title yet; the same screen changes
over 0.3 s to two hands holding a keyboard sideways under a red "no" sign, with **"Do not hold
your keyboard sideways."** in the game's own font. 2 (or A) again goes on: to the update screen
or the title. With a Wii Remote as player 1, the boot is unchanged. Code: `runtime/rt_bootscreen.c`
(hooked from `rt_menu.c`'s layout hooks and input filter, and `rt_lyt.c`'s `hle_DoSceneChange`).

**How the second screen gets its turn.** When the console's screen is done, the boot asks for the
title (`DoSceneChange`, scene 6, every frame until it goes through). `boot_kb_hold()` holds that
request while the keyboard screen is up, just as the update screen does after it. The boot's fade
to black comes with the scene change, so the layout stays up. It is skipped in deterministic test
runs (`RT_DETERMINISTIC`) unless `RT_BOOT_KEYBOARD=1`.

## The layout (`RT_LYTDUMP=1`)

```
N_contInfo_00
  W_contInfo_00                  the striped panel
  T_contInfo_00, T_contInfo_01   the text's shadow, then the text (font size 32, 1024-byte buffers)
  W_yokomochi_00   300 x 180     the picture box: white inside, green frame 4 units
    P_yoko_00      384 x 224     the picture, scale 0.7, a 384x220 RGB5A3 texture; drawn after
                                 the box, so over its frame
  N_btn_00                       the "2" button
```

What changes (keyboard only):

- **`P_yoko_00`.**
  - Its texture becomes `ui/graphics/boot/keyboard_sideways.png`: a GX RGB5A3 texture (16 bits,
    3-bit alpha). It sits in guest memory in the game-picture region 0x81870000-0x81900000
    (`gs_ui_mem_borrow` in `rt_gamesetup.c`), which is free until Game Select. The first game
    portrait then takes the region back and tells the boot screen (`boot_screen_mem_lost`).
  - The pane is centred on the box, and its size (divided by its 0.7 scale) is the box's inside
    width, 292 units, by whatever height the PNG's shape gives. Its vertex colours become white.
- **The text.** Both text boxes get "Do not hold your\nkeyboard sideways." in their own buffers,
  so it is the game's font, size, outline and shadow.

## The picture (`tools/make_boot_picture.py`)

`ui/graphics/boot/keyboard_sideways.png` is 584 x 400 pixels, 2 per layout unit:

- **The drawing** fills the middle 292 x 172 units, the box's inside.
- **The rows above and below it** (14 units each) are transparent except for the sign. That
  part shows over the box's green frame.
- **The red sign** is centred on the box. Its radius is 95.6 units, so it reaches past the box at
  the top and bottom, and its line is 8.75 units.

It is made from the drawing `tools/art/keyboard_sideways_source.png`:

1. **The keys are redrawn** alike: a 75% keyboard, 6 rows of 15 units, turned so the space bar is
   on the right. They are warped onto the drawing's keyboard face, with the hands kept on top.
   They are mechanical keycaps (`--keys 3d`, the default): darker sides (the skirt) and a lighter
   top set in, more on the side facing the viewer. The top has a grey outline, so it stays
   thinner than the key's own. `--keys flat` gives one outline per key.
2. **The console picture's colours.** Skin (255, 231, 189) is sampled from the console's picture
   and keeps a little of the drawing's shading. Lines are (56, 47, 40).
3. **Every line is grown** so it shows as thick as the console's lines, which are about 1.75 px at
   640x456. The black background becomes white.
4. **A window of the drawing** fills the box: the hands are as big as the console's (palm with
   thumb measured on both, then x0.85 x0.85 at the user's request). Their fingertips are 12% down
   the box and the arms run off its bottom, so the top of the keyboard is cut off.
5. **The sign** is 1.2 x the box-fitted size, with a thinner line, as the user chose.

```
python tools/make_boot_picture.py [--size 0.7225] [--sign 1.2] [--sign-line 0.05] [--lines 5] [--keys 3d|flat]
```

Each option changes one of these. The script needs Pillow, numpy and opencv-python.

**The picture can still be changed.** The game reads the PNG at start, so any 584 x 400 PNG with
the same layout works: drawing in the middle 172 rows' worth (344 px), anything above and below
over the frame. The script only makes this one, from the user's drawing. It can be any size up to
1024 x 1024, as long as its width matches the box and its height keeps the box's middle.

## The update screen uses this layout too

When the game checks for updates, the boot waits after this screen (`update_boot_hold`,
docs/UPDATES.md). The boot's fade to black comes with the scene change, which is waiting, so
the layout is still on screen. The update screen is now this layout itself, with the real panel
`W_contInfo_00`: the game's rounded window with its stripes. Before, it was a drawn copy of the
panel, slightly off, with square corners.

- **Over 0.3 s** the picture box (`W_yokomochi_00`, `P_yoko_00`) and the old text fade out, the
  "2" button goes, and the update's text fades in. The text is the layout's own boxes, so it is
  in the game's font: one line, or two broken near the middle when longer than ~25 letters, and
  smaller if a line is still too long. The text moves up to y = -40 (`UPD_TEXT_Y`).
- **`rt_menu.c` `update_screen` draws over it:** the spinner, or the download bar and "12.3 of
  61.9 MB - 4.2 MB/s", where the picture box was. It also draws the version where the "2" button
  was, "1: Skip (download it next time)" at bottom left, and the reason under "Update failed".
- **Positions.** A layout point (x, y) is (320 + x * 480/456, 240 - y * 480/456) in the overlay's
  640 x 480.
- **Should the fader be black** when the wait starts, `boot_update_frame` (called from
  `hle_DoSceneChange`) fades it back in.
- **At the end** the overlay fades to black as before, and the title starts.

Testing: `RT_UPDATE_FADE=3` makes the change take 3 s (to look at it). An `update.ini` pointing at
a closed port shows "Couldn't reach the update server". A local copyparty with a published
version shows the download (docs/UPDATES.md).

## Testing

- `RT_BOOT_KEYBOARD=1` / `0` forces the keyboard screen on or off (normally: player 1's controller
  decides; deterministic runs: off).
- `RT_BOOT_DEBUG=1` logs the picture pane's size, scale, texture and material.
- `RT_BOOT_TRACE=1` logs every 15th frame whether the layout is drawn (and the update screen's state).

Verified (Linux, GL/llvmpipe):
- The order: the console's screen, 2, then the keyboard screen (cross-fade), 2, then the title;
  with an update server set, the update screen comes after the keyboard screen ("Checking for
  updates", "Couldn't reach the update server"), then the title.
- The keyboard screen fades in and out like the original, with the sign over the box's frame and
  the text in the game's font.
- With `RT_BOOT_KEYBOARD=0` the console's screen is unchanged.
- Game Select's portrait still loads afterwards, in the same memory.
- The update screen in the real panel: "Checking for updates" with the spinner, then "Couldn't
  reach the update server" on two lines. With a local copyparty serving 0.1.1 (25 MB, throttled):
  "Downloading version 0.1.1", the bar and the speed, "1: Skip", the version. The change from
  the keyboard screen at 3 s was checked frame by frame.
