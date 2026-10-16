/* smb1_physics.h - the player movement of Super Mario Bros. (NES), for the retro characters.
 *
 * A port of the player physics in the SMB1 disassembly (SMBDIS.ASM: PlayerCtrlRoutine,
 * PlayerMovementSubs, PlayerPhysicsSub, OnGroundStateSub / JumpSwimSub / FallingSub /
 * ClimbingSub, GetPlayerAnimSpeed, ImposeFriction, MovePlayerHorizontally, ImposeGravity),
 * byte for byte: 8-bit speeds with their fractional "move force" bytes, the same tables, the
 * same order of operations, the same quirks. The variables keep the disassembly's names.
 *
 * It knows nothing about the level: each frame the host (the NSMBW player it drives) tells it
 * what the collision found (landed, bumped a ceiling, hit a wall, left the ground, stomped...)
 * and it returns how far to move. Units are the same in both games: 1 pixel = 1 unit, a block
 * is 16, 60 frames per second. SMB1's y grows downwards; so do the values here (the host flips).
 */
#pragma once
#include <stdint.h>

/* SavedJoypadBits */
enum { SMB1_A = 0x80, SMB1_B = 0x40, SMB1_SELECT = 0x20, SMB1_START = 0x10,
       SMB1_UP = 0x08, SMB1_DOWN = 0x04, SMB1_LEFT = 0x02, SMB1_RIGHT = 0x01 };
/* Player_State */
enum { SMB1_ON_GROUND = 0, SMB1_JUMP_SWIM = 1, SMB1_FALLING = 2, SMB1_CLIMBING = 3 };
/* PlayerFacingDir / Player_MovingDir */
enum { SMB1_FACE_RIGHT = 1, SMB1_FACE_LEFT = 2 };

typedef struct smb1_player {
    /* set by the host */
    uint8_t PlayerSize;            /* 0 big, 1 small */
    uint8_t SwimmingFlag;          /* in water */
    uint8_t AreaTypeWater;         /* the area counts as a water level (AreaType == 0) */
    uint8_t Whirlpool_Flag;
    uint8_t WaterTop;              /* at the water's top: can't swim higher (SMB1: Player_Y_Position < $14) */
    uint8_t PlayerChangeSizeFlag;  /* growing/shrinking: no movement */
    uint8_t JumpspringAnimCtrl;    /* on a springboard: no jumping, no horizontal movement */
    uint8_t Entrance;              /* scripted walk-in (GameEngineSubroutine 7): pipe intro speed */
    /* state (the disassembly's RAM) */
    uint8_t Player_State;
    uint8_t CrouchingFlag;
    uint8_t Player_X_Speed;        /* signed, 1/16 pixel per frame */
    uint8_t Player_X_MoveForce;    /* fraction of speed (friction) and of position (movement) */
    uint8_t Player_Y_Speed;        /* signed, pixels per frame */
    uint8_t Player_Y_MoveForce;    /* 1/256 of the vertical speed */
    uint8_t Player_YMF_Dummy;      /* 1/256 of the vertical position */
    uint8_t VerticalForce, VerticalForceDown;
    uint8_t PlayerFacingDir, Player_MovingDir;
    uint8_t Player_XSpeedAbsolute, RunningSpeed;
    uint8_t MaximumLeftSpeed, MaximumRightSpeed;
    uint8_t FrictionAdderLow, FrictionAdderHigh;
    uint8_t RunningTimer, JumpSwimTimer, ClimbSideTimer, SideCollisionTimer;
    uint8_t SavedJoypadBits, A_B_Buttons, PreviousA_B_Buttons, Left_Right_Buttons, Up_Down_Buttons;
    uint8_t Player_CollisionBits;  /* bit 0: may move right, bit 1: left, (climbing: 2/3 down/up) */
    uint8_t JumpOrigin_Y_Position, DiffToHaltJump;
    uint8_t PlayerAnimTimerSet;
    uint8_t Player_Y_Position;     /* low byte of the vertical position (jump height test) */
    /* animation (PlayerGfxHandler) */
    uint8_t PlayerAnimTimer, PlayerAnimCtrl;
    uint8_t FrameCounter;
} smb1_player;

typedef struct {
    int dx, dy;                    /* pixels moved this frame (dy: down is positive) */
    int jumped;                    /* a jump (or swim stroke) started this frame */
} smb1_motion;

void smb1_init(smb1_player *p, int small, int facing);
/* one frame: buttons are SavedJoypadBits (SMB1_A | SMB1_RIGHT ...) */
void smb1_frame(smb1_player *p, uint8_t buttons, smb1_motion *m);

/* what the host's collision found this frame (call after moving the player) */
void smb1_ground(smb1_player *p, int on_ground);      /* PlayerBGCollision: lands, or starts falling */
void smb1_head_bump(smb1_player *p, int solid);       /* hit a ceiling: solid = 1, a block that bounces = 0 */
void smb1_wall(smb1_player *p, int dir);              /* ImpedePlayerMove: blocked on the right (1) / left (2) */
void smb1_stomp(smb1_player *p);                      /* bounced off an enemy (SBnce) */
void smb1_spring(smb1_player *p, int a_pressed);      /* launched by a springboard */
void smb1_platform_top(smb1_player *p);               /* standing on a moving platform */

/* Which picture to show (PlayerGfxHandler / ProcessPlayerAction / AnimationControl): call once
 * per frame after smb1_frame. `anim` is the frame within the animation (walk 0-2, swim 0-2,
 * climb 0-1); `kick` is set on the frames where SMB1 swaps in the kicking feet while swimming. */
enum { SMB1_GFX_JUMP, SMB1_GFX_SWIM, SMB1_GFX_STAND, SMB1_GFX_SKID, SMB1_GFX_WALK,
       SMB1_GFX_CLIMB, SMB1_GFX_CROUCH, SMB1_GFX_FIRE };     /* PlayerGfxTblOffsets order */
typedef struct { int gfx, anim, kick; } smb1_gfx;
void smb1_graphics(smb1_player *p, smb1_gfx *g);
