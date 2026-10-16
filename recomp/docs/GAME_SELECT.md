# Game Select

Title menu > Singleplayer / Multiplayer opens **Game Select**, then File Select, then the
character select (`runtime/rt_gamesetup.c`). Game Select is the game's file select screen with
one card in the middle. Later it will list level and map packs too (TODO.md).

## Game portraits

Each game's card shows its portrait:

```
ui/graphics/gameportrait/<game id>.png       nsmbw.png = New Super Mario Bros. Wii
```

- Any size and shape; a box-art cover (about 5:7) works well. Transparent pixels stay
  transparent.
- The card takes the picture's shape. It stays as tall as before (the picture area is 155
  layout units), its width follows the picture, and the white border stays 5 units all round.
  The select cursor (the four orange corners) follows.
- Without a picture the card is the plain one with the game's name ("NSMBW").
- File Select's cards are untouched; leaving Game Select puts back every size, texture and
  colour the card had.

The game ids are in `GAMES[]` in `rt_gamesetup.c`. For now there is only `nsmbw`.

## How (`rt_gamesetup.c`, "game portraits")

- **Loading.** The PNG is loaded the first time Game Select shows. It is scaled down (area
  average) to fit 512 x 512, in whole 8 x 8 tiles, never scaled up, and compressed to GX CMPR
  (DXT1: principal-axis endpoints plus one least-squares pass; 3 colours + transparent in a 4x4
  block that has transparent pixels). It goes into our guest memory at
  **0x81870000-0x81900000**, which is free: past the 8-player module's loader scratch, before
  Retro Mario's sheets at 0x81900000. A 512-high cover takes 90 KB, so about six fit.
- **The picture.** The card's `P_NumBase_01` normally draws an 8x8 I8 texture tinted by vertex
  colours (the blue-green gradient). Its first TexMap (material +0x40) is pointed at the
  portrait (format 14, CMPR) and its four vertex colours (Picture +0xD8) are set to white. Both
  are saved and put back.
- **The shape.** Before the matrices are computed (`gs_layout_calc`), the widths (+0x4C) of
  the card's panes are reduced by `155 x (1 - width/height)`. Those panes are
  `N_dateFile_00`, `P_NumBase_01`, `P_Shine_00`, `W_dateFile_00`, `W_shadow_00`,
  `P_selectBase_00` and `W_select_00`. `N_NewFile_00` (the name) is hidden.
- **The cursor.** `dGameCom::SelectCursorSetup` copies the pane's size into `dSelectCursor_c`
  when the cursor is put on it: per cursor 612 bytes, +772 on, +773 the pane's name, +876/+880
  width/height. The instance is at 0x8042A2C8 (r13 - 21432; r13 = 0x8042F680). A cursor on a
  card pane whose width we changed gets the new width, and the old one back afterwards.

The texture lives in guest memory, so save states and the online game carry it like
everything else. The host's state has it, and a client gets it from there.
