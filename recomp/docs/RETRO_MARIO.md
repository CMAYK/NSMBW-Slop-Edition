# Retro Mario (SMB1)

A character with the look and the movement of Super Mario Bros. (NES) that plays in NSMBW
levels next to the normal characters and interacts with everything they do.

## Decisions (user, 2026-10-13)

| Topic | Decision |
|---|---|
| Art | User draws the sprites. Templates (white boxes per frame): `ui/retro/mario/*.png`, layout in `frames.json`, guide in `guide/` (`tools/make_retro_templates.py`). |
| Physics | Faithful SMB1, ported from the disassembly the user provided (`../SMB1Disassembly/SMBDIS.ASM`). No wall jump / spin jump / ground pound. |
| Power-ups | All NSMBW power-ups (sheets: small, super, fire, ice, propeller, penguin, mini). |
| Selection | An extra character in the character select. |
| Drawing | One textured plane, 1 tile (16 units) wide: 16x16 small, 16x32 big. 1 sprite pixel = 1 game unit. |

## Scale

Both games use 16-unit blocks and run at 60 frames per second, so SMB1's numbers carry over
unchanged: 1 SMB1 pixel = 1 NSMBW unit, speeds in pixels per frame are units per frame. (A
tile *looks* about 24-27 pixels big on screen at 640x480 - that is only the camera.) SMB1's y
grows downwards, NSMBW's upwards: the glue flips the sign.

## Pieces

1. **`runtime/smb1_physics.c/.h` - done.** The SMB1 player movement, translated routine by
   routine from the disassembly (PlayerPhysicsSub, ImposeFriction, MovePlayerHorizontally,
   ImposeGravity, JumpSwimSub, ClimbingSub, ...) with the original 8-bit speeds, fractions,
   tables and quirks. It knows nothing about the level: each frame it gets the buttons, returns
   how far to move, and is told what the collision found (`smb1_ground`, `smb1_head_bump`,
   `smb1_wall`, `smb1_stomp`, `smb1_spring`). Tests: `tools/test_smb1_physics.c` (walk 1.5 and
   run 2.5 px/frame, standing jump 66 px, running jump 83 px, terminal fall 4 px/frame, no air
   friction, skid...): `cc -Iruntime tools/test_smb1_physics.c runtime/smb1_physics.c`.
2. **The character in the character select - done.** Retro Mario (SMB1) is character 10 of
   the roster (`docs/CHARACTERS.md`, `runtime/rt_chars.c`); the player slot that picks it is
   retro (`chars_is_smb1(n)`). Its body is Mario's when free (else a free one). For testing,
   `retro_mario = 1` in settings.ini makes every Mario retro.
3. **Driving an NSMBW player with the SMB1 physics - done** (`runtime/rt_retro.c`). The retro
   character is a real NSMBW player (dAcPy_c), so enemies, blocks, items, pipes and other
   players work as usual. Hooks (`hle_funcs.txt`):
   - `fn_80056E40` (daPlBase_c's speed + position step, called by executeMain between the
     state's execute and the collision check): in the movement states (Walk, Jump, Fall, Land,
     Crouch, SitJump, Turn, Swim; not in a cutscene) it runs `smb1_frame` with the player's
     buttons. **On the ground** NSMBW's own step then moves the player at the SMB1 speed
     (mSpeedF = mMaxSpeedF = SMB1 x speed / 16, instant acceleration): slopes, moving and tilting
     floors work as for every other character (2026-10-14: the old "move by SMB1's pixels and
     push down 2 + |dx|" clipped into tilting floors). **In the air** the player is moved by
     SMB1's speeds directly (x speed / 16; y speed + its 1/256 fraction): smooth, the same
     distance as SMB1's whole-pixel steps on average (those judder against NSMBW's camera).
     Taking over from NSMBW carries the speed over.
   - `fn_801438A0` (dAcPy_c::executeMain): after it, NSMBW's collision result goes back to
     SMB1 - ground (BgCross bit 0), ceiling (bit 1), walls (detected as "the collision moved
     the player back from where SMB1 sent it"; the BgCross wall bits don't say the side).
   - `dAcPy_vf3FC` (setJump): called from outside the player's own update it is a bounce -
     stomp (`smb1_stomp`) or, faster than 5, a spring (`smb1_spring`).
   - Moves SMB1 doesn't have return "no": `fn_801285D0` (start a wall slide),
     `fn_801266B0` (setHipAttackAction), `fn_8005E780` (triggerShakeJump; the propeller suit
     still spins).
   All other states (pipes, doors, vines, damage, death, flagpole, cutscenes) are NSMBW's.
   - No particles: `dEf::createPlayerEffect` / `createPlayerEffect_change` (all six) do nothing
     for a retro player (dust, skid smoke, landing, power-up sparkles...).
4. **Drawing the sprite - done** (`rt_retro.c`). dAcPy_c::draw (`fn_80145A60`) doesn't draw the
   3D model; it enters a draw proc (an `m3d::proc_c` with a vtable whose drawOpa is native code,
   `rt_register_native`) in the game's 3D scene, which draws one textured quad: 1 tile wide
   (frame box from frames.json: 16x16 small, 16x32 big), feet at the player's position, the
   game's camera matrix, depth tested, mirrored when facing left. The picture comes from
   `smb1_graphics` (PlayerGfxHandler / AnimationControl: walk cycle speed from the run speed,
   skid, jump, swim with kicking feet, climb); in NSMBW-driven states a picture is picked from
   the state name (death, vine, hang, flagpole, jump/fall, walk). Sheets are loaded from
   `ui/retro/mario/<form>.png` into guest memory at 0x81900000 (RGBA8; the renderer's texture
   range was widened to 0x81A00000 for it). A sheet that is still only white boxes is skipped
   (big forms fall back to the super sheet, then to the small one).
   Frames: `frames.json` holds each frame's position; the painted small sheet's positions were
   measured from the art (row 2: 8 swim frames = 3 SMB1 swim pictures, each with and without
   the kicking feet, + 2 spare; climb 1-2). SMB1's walk pictures 1-3 are walk2, walk1, walk3.
5. **No 3D model anywhere - done** (`rt_retro.c`, user 2026-10-14: "all instances of the NSMBW
   mario model should be replaced with the plane of mario's graphic"). Every other player model
   (world map, the character select figure, anything else) is drawn through
   `dPyMdlMng_c::draw` (`draw__11dPyMdlMng_cFv`, hooked): a retro character's model is not
   drawn; a draw proc per model (its own vtable, native drawOpa) draws the sprite instead -
   a quad facing the camera (built in view space), feet at the model matrix's position, sized
   by the model matrix's scale against the model's level scale (`0x80370DA8[model +0x152]`), so
   it is as big as the model was. Which models: the model's +0x150 is the player number in
   levels (scene type +0x180 = 0) and the player *type* elsewhere (1 world map, 2 select
   figure): a model is retro when the slot with that type plays SMB1 (`chars_is_smb1`). Form:
   `daPyMng_c::mPlayerMode[type]`. Facing, left or right only (rules from the user):
   world map - from the model's facing in view space, kept while it walks up or down;
   character select - the way that player last pressed left or right (`chars_select_facing`,
   rt_chars.c; also the presses that walked the cards); elsewhere - the model's facing.
   Picture: walk cycle while it moves, jump while it rises or falls, else standing. The procs
   are dropped with their model (`__dt__11dPyMdlMng_cFv`, hooked) - they live on the scene heap.
   Not models (still NSMBW's pictures): the 2D Mario head icons of the HUD / course-in / file
   select (`x05`).
6. **More retro characters** - the roster (`docs/CHARACTERS.md`) has Retro Mario (SMW) and
   (SM64) after SMB1; the same split applies (a movement port + a sprite set in
   `ui/retro/<name>/`). (Retro Luigi was on the first list; not on the 2026-10-13 roster.)

7. **What SMB1 Mario can't do - done (2026-10-15, user's list)** (`rt_retro.c`). The player state
   manager's `changeState` (`fn_80047430`; r3 = manager, r4 = the sStateID, whose +4 is its name)
   refuses for a retro player: `Slip` (sliding down slopes), `Crouch` / `SitJump` while small or
   mini (only big SMB1 Mario ducks), `LiftUp` (lifting another player), `RideYoshi`. Also:
   `fn_8012E400` = dAcPy_c::spinLiftUp (picking up / carrying shells, blocks, players) says no;
   `fn_8014eb70__9daYoshi_cFP7dAcPy_ci` (0x8014EA30, Yoshi lets a player get on) says no.
   **Yoshi bug** (a Yoshi turned into SMB1 Mario): Yoshi's model manager has the rider's number
   at +0x150, so it was taken for the player's model; models with chara id (+0x151) 5 (Yoshi)
   are never retro now.
8. **No particles anywhere - done (2026-10-15)**. Besides the dEf::createPlayerEffect hooks, every
   effect made while a retro player's executeMain runs is dropped at the effect library
   (mEf::createEffect, effect_c / levelEffect_c::createEffect - 7 hooks, `fx_block`). World map:
   the player effects of dWmEffectManager_c::playEffect (`Wm_cs_mrlandsmoke`, `footsmoke`,
   `footsand01/02`, `footsoil`, `footice`, `jumpwater`, `landwater`, `comnlandsmoke`,
   `mrsandlandsmoke`) are dropped near a retro map sprite (or, made without a position, while one
   is on the map). `RT_EFFECT_LOG=1` logs every effect and what was dropped.
9. **Goal pole, the SMB1 way - done (2026-10-15)**. From `FlagpoleSlide` / `PlayerEndLevel` /
   `ClimbingSub` / `HandleClimbing` in the disassembly: `fn_8013FCE0` = dAcPy_c::setDemoGoal
   (landPos, castleX) starts a script (NSMBW's own goal sequence goes on underneath and drives
   the course clear; only the sprite follows the script): slide at 1 pixel a frame (the 1/256
   accumulator gets $FF per frame: SMB1 climbs down at 255/256 px, not NSMBW's 1.9), climbing
   pictures alternating every 8 frames, held 7 units left of the pole facing right
   (ClimbXPosAdder); stop 2 units above the pole's base (SMB1: $9E vs the block at $B0); wait for
   the flag (61 frames from the top at 2 px/frame, minus the slide); swing to the other side
   (ClimbAdderLow +14, facing left); after ClimbSideTimer ($18) let go (+4) and walk right with
   the SMB1 physics (AutoControlPlayer with right; ground from dBc_c::checkGround 0x800757B0)
   until the castle (setDemoGoal's castleX, mWarpPos.x +0x3FC), where he's gone. Drawn in the
   translucent pass without the depth test: over the pole, in front of the other players' poses.
10. **Vines - done (2026-10-15)**. On an ivy vine (`Vine` state, mBc.mFenceType +0x2B8 = 0) left /
   right no longer move NSMBW's player; they swing him to that side (once per $18 frames, like
   ClimbingSub). He is held 7 units left or right of the middle of the tile he caught it in,
   facing the vine; climbing pictures alternate every 4 frames going up, 8 going down. Nets and
   rock walls stay NSMBW's.
11. **Sprite sheets (2026-10-15)**: every frame is copied into its own cell of the texture, so
   frames painted closer together than the frame box (the small sheet's swim row is 14 px apart)
   no longer show a sliver of the neighbour (a column run touching the box edge that goes on
   outside it, and isn't the frame's biggest, is dropped). A frame box with nothing painted (or a
   template's white box) counts as missing (its fallback is used). **Big Mario** (`super.png`,
   user's art) is used for every big form until those have their own sheets. Swimming: 4
   pictures, each with and without the kicking feet (swim1/2, 3/4, 5/6, 7/8), a new picture each
   time SMB1's swim animation steps. `RT_RETRO_DUMP_SHEETS=dir` writes the cells as .ppm.
12. **Sounds - done (2026-10-15)** (`rt_sfx.c`: WAVs from `ui/retro/mario/sfx`, resampled to
   32 kHz and mixed into the game's audio blocks; `retro_sfx_volume` in settings.ini, 0-100,
   default 70). Only for what the SMB1 player does; NSMBW's own sound for it (and every sound of
   the player's own sound object: footsteps, voice) isn't played (hooks on
   nw4r::snd::SoundStartable::detail_StartSound / detail_HoldSound, IDs from the decomp's
   sound_list.h):
   jump / jumpsmall and stompswim (swim stroke, stomp) from the SMB1 physics; bump / brick /
   item / coin when its head hits a block (NSMBW's SE_OBJ_BLOCK_BREAK / ITEM_APPEAR / GET_COIN
   within 6 frames says which, else bump); coin (a coin it touches), 1up, fireball
   (SE_PLY_THROW_FIRE) while its own update runs; powerup / pipepowerdown from its form
   changing; pipepowerdown on going into a pipe; death (NSMBW's miss music isn't started:
   daPyMng_c::startMissBGM). `RT_SE_LOG=1` logs NSMBW's sounds, `RT_SFX_LOG=1` ours.

## Testing

- `tools/test_smb1_physics.c` - the physics and the picture choice, without the game.
- In the game (Linux build, headless): pick Retro Mario in the character select (or
  `retro_mario = 1` in settings.ini), then
  `RT_RETROLOG=1` (a line per frame: who moves it, state, position, SMB1 state/speeds) and
  `RT_RETRO_SCRIPT="R:40 RB:40 RBA:25 :30 L:12 A:25"` (buttons from the first frame the retro
  player can move: R/L/U/D, A jump, B run, `:n` frames), `RT_RETRO_EXIT=n` (quit n frames
  after the script). With `RT_AUTOPRESS` to get through the menus and `RT_WARP="0,0,0,0"`
  (World 1-1).

## Open

- World map walking (left / right / up / down along paths) checked only by logic: the test save
  had no open path. Check the sprite's size against the map model's look.
- HUD / course-in / file select head icons for the retro character (2D pictures, user's call).
- Sprites for the other forms (fire, ice, propeller, penguin, mini) - user (big: done).
- **Underwater bubbles still there**: the small bubbles that rise near SMB1 Mario after a swim
  stroke are not made through the effect system - they stay with every effect switched off
  (`RT_NO_EFFECTS=1`, testing) - nor by the player's own code (no effect, actor or joint lookup
  while swimming). Most likely the water itself (AC_BG_WATER / the water entry manager) makes
  them for anything moving underwater. To do: find that code in d_basesNP.
- Not verified in play: vines (no test level found), Yoshi, carrying, the world map's smoke with
  a retro map player, 1-up and fireball sounds (who collected the 1-up: only while its own
  update runs).
- SMB1 pictures in more NSMBW states: carrying, throwing, kicking, power-up change (grow),
  shooting fireballs, pipe entry (front/back), victory.
- Verify in play: stomping, springs, swimming, vines, moving platforms, slopes.
- Testing switch `RT_RETRO_STAR=1`: the retro player stays invincible (star timer), to walk a
  level in a scripted test without dying.
