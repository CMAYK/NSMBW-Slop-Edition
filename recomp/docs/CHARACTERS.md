# Characters (more than 8, any number of players up to 8)

Goal (user, 2026-10-13): characters are a list (data), separate from the 8 player slots, so
the roster can grow past 8 - 12 now, room for more later.

## The roster

In order. Only the first five are on the title screen.

| # | Character | Today (8-player module type) | Needs |
|---|---|---|---|
| 1 | Mario | MARIO (0) | - |
| 2 | Luigi | LUIGI (1) | - |
| 3 | Ala Gold | YELLOW_TOAD (3) - *assumed: Ala Gold = the yellow Toad* | name, portrait |
| 4 | Buckenberry | BLUE_TOAD (2) - *assumed: Buckenberry = the blue Toad* | name, portrait |
| 5 | Toadette | TOADETTE (4) | portrait (icon for now) |
| - | (Purple Toadette) | PURPLE_TOADETTE (5) | dropped from the roster |
| 6 | Toadsworth | BLACK_TOAD body, Toad addon textures | portrait (icon for now), voice |
| 7 | Daisy | ORANGE_TOAD body, Rosalina/Daisy mod model | icons, voice, world map |
| 8 | Rosalina | PURPLE_TOADETTE body, Rosalina/Daisy mod model | icons, voice, world map |
| 9 | Goomba | borrows a body; Goomba mod model waits for a 9th | own body, icons |
| 10 | Retro Mario (SMB1) | MARIO + retro look/physics | portrait; sprites: small done, other forms to draw |
| 11 | Retro Mario (SMW) | new retro style | SMW physics port, sprites |
| 12 | Retro Mario (SM64) | new retro style | design (SM64 movement in 2D), sprites or model |

## What has a fixed count today

The 8-player module has `CHARACTER_COUNT = PLAYER_COUNT = 8` and ~46 places that use it, in
17 files, compiled into the module: the player manager's per-character arrays
(`daPyMng_c::mPlayerType/mPlayerMode/mCreateItem/mRest/mCoin`, indexed by character), the
character select (`dNumberOfPeopleChange_c`, `dCharacterChangeSelect*`), the save file (JSON,
per-character lives/power-ups), HUD icons, world map figures, the midway flag, Multi Course
Select. Plus ~150 patches in the game itself that went from 4 to 8 - some count players, some
characters.

## Done: the list and the character select (step 1, 2026-10-14)

`runtime/rt_chars.c`. The roster above is a table (`ROSTER`): id (stable, for files and later
saves), name, **body** (one of the 8 player types the game knows: model, voice, per-type lives
and power-up) and **style** (normal, or a retro style, `rt_retro.c`). Each player slot remembers
the character it picked (`chars_of_player(n)`; derived from its type when it never picked).

- **Character select** (user: keep NSMBW's cards): left/right walks all 12 characters. The cards'
  code counts `mOption` down from 4 (character = 4 - mOption), now down to 4 - 11. Natively:
  SelectWait (mod_fn_2_3170), calcContentsIcon (2600: the card gets `100 + 2 * character +
  taken`), isCharacterLocked (2510), initOption (26F0: cursor starts on the player's character),
  initDecidedCharacter (26D0: picks the body, remembers the character), readyContents (2770);
  the arrows' range ends (-3 / 4 in the module's code, mod_fn_2_E90 / 1290) are mapped.
- **Portraits** (the missing-portrait bug): dispNextConPicture (mod_fn_2_5AB0) natively. Mario,
  Luigi, Ala Gold (yellow Toad) and Buckenberry (blue Toad) use the layout's pictures; every
  other character shows `ui/characters/<id>/portrait.png`, put into the card's Mario picture
  pane by pointing its TexMap at our texture (TexMap = material + 0x6C via the pointer at
  material + 0x40; RGBA8 copies at 0x81A00000+, renderer texture range raised to 0x82000000).
  A portrait.png for one of the first four replaces the game's picture.
  Where they come from (`tools/import_char_mods.py`, run by build.py when game\mod has a newer
  zip): Daisy, Rosalina - the Rosalina and Daisy mod's select portraits; Goomba - the Goomba
  mod's; Toadette, Toadsworth - their HUD icons (48x48, scaled; no portrait exists); the retro
  characters - placeholders with the name (`tools/make_char_portraits.py`).
  The card has two faces (the character, and the next one sliding in during a move), both
  with their character's picture, as the module animates them. They are clipped to the card by
  the game itself: the select base sets the contents layout's m2d clip from its
  N_scissorArea_00 pane (GXSetScissor in fn_80007010). A 4:3-mode hack in `rt_aspect.c`
  (m2d clips widened to the whole screen, meant for letterbox bands) had removed that clip, so
  a face beside the card showed; it now only widens full-width clips.
- **Bodies (8 player types) and models**: Mario, Luigi, Ala Gold (yellow Toad), Buckenberry (blue
  Toad), Toadette, Toadsworth (Black Toad), Daisy (Orange Toad), Rosalina (Purple Toadette).
  Models from the mods in game\mod (`runtime/rt_charmdl.c`, files in game\mod\characters\):
  - Toadsworth: the "8 Player Mod Toad Addon" Kinopio model (its Black Toad textures are
    Toadsworth's) - used for all Toad bodies.
  - Daisy, Rosalina: the "Rosalina and Daisy for NSMBW" Kinopio model + its own P_rcha/K_rcha
    (dress/skirt bones; its P_rcha would change Mario's animations, so only these two use it).
    Daisy is that mod's blue-Toad texture set, Rosalina its yellow one. Purple Toadette's body is
    built as a Toad-type model for Rosalina (dPyMdlMng_c::construct).
  - Not covered: their world-map figures (still the Toads'), voices (Toad / Toadette), HUD icons
    (Orange Toad / Purple Toadette), hit box (Toad's). The mods' own code (Riivolution Kamek
    binaries) isn't used.
  - Goomba (the Goomba mod's model) needs a 9th body: it borrows a free one and looks like it.
  Goomba and the retro characters borrow a body: their preferred one if free (retro: Mario's),
  else the first free one. A character is taken when another decided player has it, or has the
  body it needs - so for now Mario and Retro Mario can't both play when Retro Mario took Mario's
  body first.
- **Lives and power-up**: singleplayer - they belong to the save file: whichever character player
  1 picks gets them (kept in Mario's slot, type 0, which is what the save file and the original
  game use; copied to the picked body at the pick and back before saving: `prepareSave` hook).
  Multiplayer - per character (per body).
- **Retro Mario (SMB1)** is picked here (the Options switch is gone; `retro_mario = 1` in
  settings.ini still makes every Mario retro, for testing). SMW / SM64 styles: not made yet -
  they play as their body for now.
- **Title screen**: the first five characters only (rt_title.c -> `chars_title_players`).

## Next

1. **Per-character data instead of per-type** (lives, power-up, items): today they belong to
   the body, so a retro character shares Mario's lives. Port `daPyMng_c`'s arrays to native,
   indexed by character.
2. **More than 8 bodies**: widen PLAYER_TYPE past 8 (module arrays + the game's ~150 patched
   spots, `CHARACTER_COUNT`) so every character has its own type; then the "taken" rule is
   only "same character", and new models plug in per character.
3. **Save format v2**: per-character data keyed by character id; the character each player
   slot picked is saved (today it lives for the session; a fresh start derives it from the type).
4. **New bodies / models**: Toadsworth, Daisy, Rosalina, Goomba (assets needed).
5. The retro characters on the select card after deciding: today the body's 3D figure jumps
   on the card; should be the sprite.

## Original plan (2026-10-13)

1. **Character table (native, data).** `runtime/rt_chars.c`: an ordered list - id (stable
   string, used in saves), display name, *body* (which NSMBW player model/voice/physics: one of
   the module's 8 types, later new models), *style* (normal / retro-smb1 / ...), title-screen
   flag, portrait / icon files. Up to 16 entries. The 12 above, in that order.
2. **Players pick characters, not types.** Each of the 8 player slots holds a character index.
   The game code keeps seeing a PLAYER_TYPE (the character's body); everything per-character
   that the game must not mix up (lives, power-up, items) moves to native arrays indexed by
   character, ported from the module (`daPyMng_c` first).
3. **Character select (native port).** Walk the whole list on NSMBW's cards, portraits from
   files (`ui/characters/<id>/portrait.png`), lock/"taken" rules per character.
4. **Save format v2.** Per-character data keyed by character id, so adding characters never
   breaks a save; v1 (module) saves are read and converted.
5. **New bodies.** Toadsworth, Daisy, Rosalina, Goomba need models (+ animations the player
   code uses) - assets to be found/made; until then they can't be played. The retro styles use
   an existing body (Mario) and draw sprites (`docs/RETRO_MARIO.md`).
6. **Title screen** shows the first five only (Mario, Luigi, Ala Gold, Buckenberry, Toadette).

## Missing portraits (bug report 2026-10-13) - fixed 2026-10-14

Not a porting bug: the module's character select only has portrait pictures for Mario, Luigi
and the two Toads (`characterChangeSelectContents_05.brlyt`: `Im_plofileMario/Luiji/KinoB/KinoY`);
for Toadette, Purple Toadette, Orange and Black Toad it showed nothing. Now: portrait files per
character (placeholders until painted).
