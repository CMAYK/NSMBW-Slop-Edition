/* smb1_physics.c - the player movement of Super Mario Bros. (see smb1_physics.h).
 *
 * Every routine below is the disassembly routine of the same name, translated instruction by
 * instruction (8-bit wrap-around, carries and the CMP-sets-N comparisons included). Comments
 * quote the original labels so the two can be read side by side. */
#include "smb1_physics.h"

static const uint8_t JumpMForceData[7]   = {0x20, 0x20, 0x1e, 0x28, 0x28, 0x0d, 0x04};
static const uint8_t FallMForceData[7]   = {0x70, 0x70, 0x60, 0x90, 0x90, 0x0a, 0x09};
static const uint8_t PlayerYSpdData[7]   = {0xfc, 0xfc, 0xfc, 0xfb, 0xfb, 0xfe, 0xff};
static const uint8_t InitMForceData[7]   = {0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00};
static const uint8_t MaxLeftXSpdData[3]  = {0xd8, 0xe8, 0xf0};
static const uint8_t MaxRightXSpdData[4] = {0x28, 0x18, 0x10, 0x0c};     /* (last: pipe intros) */
static const uint8_t FrictionData[3]     = {0xe4, 0x98, 0xd0};
static const uint8_t Climb_Y_SpeedData[3]  = {0x00, 0xff, 0x01};
static const uint8_t Climb_Y_MForceData[3] = {0x00, 0x20, 0xff};
static const uint8_t PlayerAnimTmrData[3]  = {0x02, 0x04, 0x07};
static const uint8_t ClimbAdderLow[4]    = {0x0e, 0x04, 0xfc, 0xf2};

#define A_BUTTON 0x80
#define B_BUTTON 0x40
static int neg(uint8_t v) { return (v & 0x80) != 0; }
/* CMP: N flag of (a - m) */
static int cmp_n(uint8_t a, uint8_t m) { return ((uint8_t)(a - m) & 0x80) != 0; }

void smb1_init(smb1_player *p, int small, int facing) {
    uint8_t *b = (uint8_t *)p;
    for (unsigned i = 0; i < sizeof *p; i++) b[i] = 0;
    p->PlayerSize = small ? 1 : 0;
    p->PlayerFacingDir = p->Player_MovingDir = facing == SMB1_FACE_LEFT ? SMB1_FACE_LEFT : SMB1_FACE_RIGHT;
    p->Player_CollisionBits = 0xff;
    p->Player_State = SMB1_FALLING;
    p->VerticalForce = p->VerticalForceDown = 0x70;
    p->DiffToHaltJump = 1;
}

/* GetPlayerAnimSpeed */
static void GetPlayerAnimSpeed(smb1_player *p) {
    int y = 0;
    uint8_t a = p->Player_XSpeedAbsolute;
    if (a >= 0x1c) { p->RunningSpeed = a; goto SetAnimSpd; }         /* SetRunSpd */
    y++;
    if (a < 0x0e) y++;
    /* ChkSkid */
    a = p->SavedJoypadBits & 0x7f;
    if (!a) goto SetAnimSpd;
    a &= 0x03;
    if (a == p->Player_MovingDir) { p->RunningSpeed = 0; goto SetAnimSpd; }
    /* ProcSkid */
    if (p->Player_XSpeedAbsolute >= 0x0b) goto SetAnimSpd;
    p->Player_MovingDir = p->PlayerFacingDir;
    p->Player_X_Speed = 0;
    p->Player_X_MoveForce = 0;
SetAnimSpd:
    p->PlayerAnimTimerSet = PlayerAnimTmrData[y];
}

/* ImposeFriction (A = left/right controller bits) */
static void ImposeFriction(smb1_player *p, uint8_t a) {
    a &= p->Player_CollisionBits;
    if (a == 0) {
        a = p->Player_X_Speed;
        if (a == 0) goto SetAbsSpd;
        if (!neg(a)) goto RghtFrict;
        goto LeftFrict;
    }
    /* JoypFrict: right button in carry; left pressed -> RghtFrict */
    if (!(a & 1)) goto RghtFrict;
LeftFrict: {
        unsigned s = p->Player_X_MoveForce + p->FrictionAdderLow;
        p->Player_X_MoveForce = (uint8_t)s;
        a = (uint8_t)(p->Player_X_Speed + p->FrictionAdderHigh + (s >> 8));
        p->Player_X_Speed = a;
        if (cmp_n(a, p->MaximumRightSpeed)) goto XSpdSign;
        a = p->MaximumRightSpeed;
        p->Player_X_Speed = a;
        goto SetAbsSpd;
    }
RghtFrict: {
        int s = (int)p->Player_X_MoveForce - p->FrictionAdderLow;
        int borrow = s < 0;
        p->Player_X_MoveForce = (uint8_t)s;
        a = (uint8_t)(p->Player_X_Speed - p->FrictionAdderHigh - borrow);
        p->Player_X_Speed = a;
        if (!cmp_n(a, p->MaximumLeftSpeed)) goto XSpdSign;
        a = p->MaximumLeftSpeed;
        p->Player_X_Speed = a;
    }
XSpdSign:
    if (neg(a)) a = (uint8_t)(~a + 1);
SetAbsSpd:
    p->Player_XSpeedAbsolute = a;
}

/* MovePlayerHorizontally / MoveObjectHorizontally: returns pixels moved */
static int MovePlayerHorizontally(smb1_player *p) {
    if (p->JumpspringAnimCtrl) return 0;
    uint8_t s = p->Player_X_Speed;
    uint8_t lo = (uint8_t)(s << 4), hi = s >> 4;
    if (hi >= 8) hi |= 0xf0;
    unsigned sum = p->Player_X_MoveForce + lo;
    p->Player_X_MoveForce = (uint8_t)sum;
    return (int8_t)hi + (int)(sum >> 8);
}

/* ImposeGravity (player: moving down only, maximum speed in max): returns pixels moved */
static int ImposeGravity(smb1_player *p, uint8_t force, uint8_t max) {
    unsigned s = p->Player_YMF_Dummy + p->Player_Y_MoveForce;
    p->Player_YMF_Dummy = (uint8_t)s;
    int dy = (int8_t)p->Player_Y_Speed + (int)(s >> 8);
    p->Player_Y_Position = (uint8_t)(p->Player_Y_Position + dy);
    s = p->Player_Y_MoveForce + force;
    p->Player_Y_MoveForce = (uint8_t)s;
    p->Player_Y_Speed = (uint8_t)(p->Player_Y_Speed + (s >> 8));
    if (!cmp_n(p->Player_Y_Speed, max) && p->Player_Y_MoveForce >= 0x80) {
        p->Player_Y_Speed = max;
        p->Player_Y_MoveForce = 0;
    }
    return dy;
}

/* MovePlayerVertically */
static int MovePlayerVertically(smb1_player *p) {
    if (p->JumpspringAnimCtrl) return 0;
    return ImposeGravity(p, p->VerticalForce, 0x04);
}

/* PlayerPhysicsSub */
static void PlayerPhysicsSub(smb1_player *p, smb1_motion *m) {
    int y;
    if (p->Player_State == SMB1_CLIMBING) {
        y = 0;
        uint8_t a = p->Up_Down_Buttons & p->Player_CollisionBits;
        if (a) { y++; if (!(a & 0x08)) y++; }
        /* ProcClimb */
        p->Player_Y_MoveForce = Climb_Y_MForceData[y];
        uint8_t t = 0x08;
        p->Player_Y_Speed = Climb_Y_SpeedData[y];
        if (!neg(p->Player_Y_Speed)) t >>= 1;
        p->PlayerAnimTimerSet = t;
        return;
    }
    /* CheckForJumping */
    if (p->JumpspringAnimCtrl || !(p->A_B_Buttons & A_BUTTON) || (p->A_B_Buttons & A_BUTTON & p->PreviousA_B_Buttons)) goto X_Physics;
    /* ProcJumping */
    if (p->Player_State != SMB1_ON_GROUND) {
        if (!p->SwimmingFlag) goto X_Physics;
        if (!p->JumpSwimTimer && neg(p->Player_Y_Speed)) goto X_Physics;
    }
    /* InitJS */
    p->JumpSwimTimer = 0x20;
    y = 0;
    p->Player_YMF_Dummy = 0;
    p->Player_Y_MoveForce = 0;
    p->JumpOrigin_Y_Position = p->Player_Y_Position;
    p->Player_State = SMB1_JUMP_SWIM;
    {
        uint8_t a = p->Player_XSpeedAbsolute;
        if (a >= 0x09) { y++; if (a >= 0x10) { y++; if (a >= 0x19) { y++; if (a >= 0x1c) y++; } } }
    }
    /* ChkWtr */
    p->DiffToHaltJump = 1;
    if (p->SwimmingFlag) { y = 5; if (p->Whirlpool_Flag) y++; }
    /* GetYPhy */
    p->VerticalForce = JumpMForceData[y];
    p->VerticalForceDown = FallMForceData[y];
    p->Player_Y_MoveForce = InitMForceData[y];
    p->Player_Y_Speed = PlayerYSpdData[y];
    m->jumped = 1;
    if (p->SwimmingFlag && p->WaterTop) p->Player_Y_Speed = 0;       /* (can't swim out of the water's top) */
X_Physics:
    y = 0;
    {
        int t = 0;                                                    /* $00 */
        if (p->Player_State != SMB1_ON_GROUND) {
            if (p->Player_XSpeedAbsolute >= 0x19) goto GetXPhy;
            goto ChkRFast;
        }
        /* ProcPRun */
        y++;
        if (p->AreaTypeWater) goto ChkRFast;
        y--;
        if (p->Left_Right_Buttons != p->Player_MovingDir) goto ChkRFast;
        if (p->A_B_Buttons & B_BUTTON) { p->RunningTimer = 0x0a; goto GetXPhy; }   /* SetRTmr */
        if (p->RunningTimer) goto GetXPhy;
ChkRFast:
        y++;
        t++;
        if (p->RunningSpeed || p->Player_XSpeedAbsolute >= 0x21) t++;  /* FastXSp */
GetXPhy:
        p->MaximumLeftSpeed = MaxLeftXSpdData[y];
        if (p->Entrance) y = 3;
        p->MaximumRightSpeed = MaxRightXSpdData[y];
        p->FrictionAdderLow = FrictionData[t];
        p->FrictionAdderHigh = 0;
        if (p->PlayerFacingDir != p->Player_MovingDir) {
            p->FrictionAdderHigh = p->FrictionAdderLow >> 7;
            p->FrictionAdderLow = (uint8_t)(p->FrictionAdderLow << 1);
        }
    }
}

static void OnGroundStateSub(smb1_player *p, smb1_motion *m) {
    GetPlayerAnimSpeed(p);
    if (p->Left_Right_Buttons) p->PlayerFacingDir = p->Left_Right_Buttons;
    ImposeFriction(p, p->Left_Right_Buttons);
    m->dx += MovePlayerHorizontally(p);
}

static void LRAir(smb1_player *p, smb1_motion *m) {
    if (p->Left_Right_Buttons) ImposeFriction(p, p->Left_Right_Buttons);
    m->dx += MovePlayerHorizontally(p);                                /* JSMove */
    m->dy += MovePlayerVertically(p);
}

static void FallingSub(smb1_player *p, smb1_motion *m) {
    p->VerticalForce = p->VerticalForceDown;
    LRAir(p, m);
}

static void JumpSwimSub(smb1_player *p, smb1_motion *m) {
    if (!neg(p->Player_Y_Speed)) goto DumpFall;
    if (p->A_B_Buttons & A_BUTTON & p->PreviousA_B_Buttons) goto ProcSwim;
    if ((uint8_t)(p->JumpOrigin_Y_Position - p->Player_Y_Position) < p->DiffToHaltJump) goto ProcSwim;
DumpFall:
    p->VerticalForce = p->VerticalForceDown;
ProcSwim:
    if (p->SwimmingFlag) {
        GetPlayerAnimSpeed(p);
        if (p->WaterTop) p->VerticalForce = 0x18;
        if (p->Left_Right_Buttons) p->PlayerFacingDir = p->Left_Right_Buttons;   /* LRWater */
    }
    LRAir(p, m);
}

static void ClimbingSub(smb1_player *p, smb1_motion *m) {
    unsigned s = p->Player_YMF_Dummy + p->Player_Y_MoveForce;
    p->Player_YMF_Dummy = (uint8_t)s;
    int dy = (int8_t)p->Player_Y_Speed + (int)(s >> 8);
    p->Player_Y_Position = (uint8_t)(p->Player_Y_Position + dy);
    m->dy += dy;
    uint8_t a = p->Left_Right_Buttons & p->Player_CollisionBits;
    if (!a) { p->ClimbSideTimer = 0; return; }                       /* InitCSTimer */
    if (p->ClimbSideTimer) return;
    p->ClimbSideTimer = 0x18;
    int x = 0;
    if (!(a & 1)) x += 2;                                             /* left pressed */
    if (p->PlayerFacingDir != SMB1_FACE_RIGHT) x++;                   /* ClimbFD */
    m->dx += (int8_t)ClimbAdderLow[x];                                /* CSetFDir */
    p->PlayerFacingDir = p->Left_Right_Buttons ^ 0x03;
}

void smb1_frame(smb1_player *p, uint8_t buttons, smb1_motion *m) {
    m->dx = m->dy = 0; m->jumped = 0;
    /* DecTimers (frame timers) */
    p->FrameCounter++;
    if (p->PlayerAnimTimer) p->PlayerAnimTimer--;
    if (p->JumpSwimTimer) p->JumpSwimTimer--;
    if (p->RunningTimer) p->RunningTimer--;
    if (p->ClimbSideTimer) p->ClimbSideTimer--;
    if (p->SideCollisionTimer) p->SideCollisionTimer--;
    /* PlayerCtrlRoutine: SaveJoyp */
    p->SavedJoypadBits = buttons;
    p->A_B_Buttons = buttons & 0xc0;
    p->Left_Right_Buttons = buttons & 0x03;
    p->Up_Down_Buttons = buttons & 0x0c;
    if ((p->Up_Down_Buttons & 0x04) && p->Player_State == SMB1_ON_GROUND && p->Left_Right_Buttons)
        p->Left_Right_Buttons = p->Up_Down_Buttons = 0;              /* (down on the ground: no walking) */
    /* PlayerMovementSubs */
    if (p->PlayerSize) p->CrouchingFlag = 0;
    else if (p->Player_State == SMB1_ON_GROUND) p->CrouchingFlag = p->Up_Down_Buttons & 0x04;
    PlayerPhysicsSub(p, m);
    if (!p->PlayerChangeSizeFlag) {
        if (p->Player_State != SMB1_CLIMBING) p->ClimbSideTimer = 0x18;
        switch (p->Player_State) {
        case SMB1_ON_GROUND: OnGroundStateSub(p, m); break;
        case SMB1_JUMP_SWIM: JumpSwimSub(p, m); break;
        case SMB1_FALLING:   FallingSub(p, m); break;
        default:             ClimbingSub(p, m); break;
        }
    }
    /* ChkMoveDir */
    if (p->Player_X_Speed) p->Player_MovingDir = neg(p->Player_X_Speed) ? SMB1_FACE_LEFT : SMB1_FACE_RIGHT;
    /* SaveAB (end of the frame's game engine) */
    p->PreviousA_B_Buttons = p->A_B_Buttons;
}

/* PlayerBGCollision: the state for this frame from the ground under the player */
void smb1_ground(smb1_player *p, int on_ground) {
    p->Player_CollisionBits = 0xff;
    if (p->SwimmingFlag) p->Player_State = SMB1_JUMP_SWIM;            /* SetPSte */
    else if (p->Player_State == SMB1_ON_GROUND || p->Player_State == SMB1_CLIMBING) p->Player_State = SMB1_FALLING;
    if (on_ground && !neg(p->Player_Y_Speed)) {                      /* LandPlyr */
        p->Player_Y_Speed = 0;
        p->Player_Y_MoveForce = 0;
        p->Player_State = SMB1_ON_GROUND;
    }
}

void smb1_head_bump(smb1_player *p, int solid) {
    if (!neg(p->Player_Y_Speed)) return;                              /* (only while going up) */
    p->Player_Y_Speed = solid ? 0x01 : 0x00;                          /* NYSpd / BumpBlock */
}

/* ImpedePlayerMove */
void smb1_wall(smb1_player *p, int dir) {
    if (dir == SMB1_FACE_RIGHT) {
        if (!neg(p->Player_X_Speed)) { p->SideCollisionTimer = 0x10; p->Player_X_Speed = 0; }
        p->Player_CollisionBits &= (uint8_t)~0x01;
    } else {
        if ((int8_t)p->Player_X_Speed < 1) { p->SideCollisionTimer = 0x10; p->Player_X_Speed = 0; }
        p->Player_CollisionBits &= (uint8_t)~0x02;
    }
}

void smb1_stomp(smb1_player *p) { p->Player_Y_Speed = 0xfc; }        /* SBnce */

void smb1_spring(smb1_player *p, int a_pressed) {
    p->Player_Y_Speed = a_pressed ? 0xf4 : 0xf9;                      /* JumpspringForce */
    p->Player_State = SMB1_JUMP_SWIM;
    p->VerticalForce = p->VerticalForceDown = 0x70;
}

void smb1_platform_top(smb1_player *p) {
    p->Player_Y_Speed = 0;
    p->Player_Y_MoveForce = 0;
    p->Player_State = SMB1_ON_GROUND;
}

/* ---- PlayerGfxHandler: the picture for this frame ----
 * ProcessPlayerAction picks the entry of PlayerGfxTblOffsets (the SMB1_GFX_ order), and for
 * the animated ones AnimationControl steps PlayerAnimCtrl each time PlayerAnimTimer runs out
 * (reloaded from PlayerAnimTimerSet, which GetPlayerAnimSpeed sets from the speed). The
 * offset is taken before the step, as in the original (pha / pla around it). */
static void AnimationControl(smb1_player *p, smb1_gfx *g, int extent) {
    g->anim = p->PlayerAnimCtrl;                                      /* GetCurrentAnimOffset */
    if (p->PlayerAnimTimer) return;
    p->PlayerAnimTimer = p->PlayerAnimTimerSet;
    uint8_t a = (uint8_t)(p->PlayerAnimCtrl + 1);
    p->PlayerAnimCtrl = a < extent ? a : 0;                           /* SetAnimC */
}
void smb1_graphics(smb1_player *p, smb1_gfx *g) {
    g->anim = 0; g->kick = 0;
    switch (p->Player_State) {
    case SMB1_CLIMBING:                                               /* ActionClimbing */
        g->gfx = SMB1_GFX_CLIMB;
        if (!p->Player_Y_Speed) goto NonAnimatedActs;
        AnimationControl(p, g, 2);                                    /* ThreeFrameExtent */
        break;
    case SMB1_FALLING:                                                /* ActionFalling */
        g->gfx = SMB1_GFX_WALK;
        g->anim = p->PlayerAnimCtrl;                                  /* (the walk frame, held) */
        break;
    case SMB1_JUMP_SWIM:
        if (p->SwimmingFlag) {                                        /* ActionSwimming */
            g->gfx = SMB1_GFX_SWIM;
            if (p->JumpSwimTimer || p->PlayerAnimCtrl || (p->A_B_Buttons & A_BUTTON)) AnimationControl(p, g, 3);
            else g->anim = p->PlayerAnimCtrl;
            break;
        }
        g->gfx = p->CrouchingFlag ? SMB1_GFX_CROUCH : SMB1_GFX_JUMP;
        goto NonAnimatedActs;
    default:                                                          /* ProcOnGroundActs */
        if (p->CrouchingFlag) { g->gfx = SMB1_GFX_CROUCH; goto NonAnimatedActs; }
        g->gfx = SMB1_GFX_STAND;
        if (!(p->Player_X_Speed | p->Left_Right_Buttons)) goto NonAnimatedActs;
        if (p->Player_XSpeedAbsolute < 0x09 || (p->Player_MovingDir & p->PlayerFacingDir)) {
            g->gfx = SMB1_GFX_WALK;                                   /* ActionWalkRun */
            AnimationControl(p, g, 3);                                /* FourFrameExtent (3 frames) */
            break;
        }
        g->gfx = SMB1_GFX_SKID;
    NonAnimatedActs:
        p->PlayerAnimCtrl = 0;
        break;
    }
    /* swimming, not standing: the feet kick on 4 frames out of 8 (SwimKickTileNum) - except on
       the small player's third swim frame, whose feet already are the kick */
    if (p->SwimmingFlag && p->Player_State != SMB1_ON_GROUND && !(p->FrameCounter & 0x04))
        g->kick = !(p->PlayerSize && g->gfx == SMB1_GFX_SWIM && g->anim == 2);
}
