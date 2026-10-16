/* rt_retro.c - the retro characters: SMB1 Mario (docs/RETRO_MARIO.md).
 *
 * A retro character is an ordinary NSMBW player (dAcPy_c): enemies, blocks, items, pipes,
 * doors and the other players treat it like any other player. Two things are different:
 *
 *  - Movement. In its normal movement states (walk, jump, fall, land, crouch, turn, swim) the
 *    player is moved by the Super Mario Bros. physics (smb1_physics.c) instead of NSMBW's:
 *    each frame the SMB1 code gets the buttons and says how far to move; the player is moved
 *    by exactly that, NSMBW's collision runs as usual, and what it found (ground, ceiling,
 *    walls) is reported back to the SMB1 code. Bounces off enemies and springs (NSMBW's
 *    setJump from outside the player's own update) become SMB1 stomps / spring launches.
 *    NSMBW moves it in every other state (pipes, doors, vines, damage, cutscenes...).
 *    The moves SMB1 doesn't have are switched off: wall slide / wall jump, ground pound and
 *    the spin jump (the propeller suit still spins).
 *  - Looks. The 3D model is not drawn; instead one textured quad, 1 tile (16 units) wide, is
 *    drawn where the player is, with the picture SMB1 would show (smb1_graphics), from the
 *    sprite sheets in ui/retro/<character>/ (layout in frames.json). It is drawn by a draw
 *    proc in the game's 3D scene (m3d::proc_c, like the game's own custom drawing), so the
 *    camera, depth and layering are the game's.
 *
 * Which players are retro: the ones whose player slot picked Retro Mario (SMB1) in the character
 * select (rt_chars.c). Testing: settings.ini retro_mario = 1 makes every Mario one.
 *
 * Hooks (hle_funcs.txt): dAcPy_c::executeMain, draw, setJump; daPlBase_c's speed/position
 * step (posMove for players); the wall slide check, setHipAttackAction, dAcPyKey_c::triggerShakeJump. */
#include "rt.h"
#include "smb1_physics.h"
#include <math.h>
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

/* ---- guest addresses (USA rev 2) ---- */
#define A_POS_MOVE_DELTA   0x8006CD00u   /* dBaseActor_c::posMove(mVec3_c&) */
#define A_TEST_FLAG        0x80056CF0u   /* daPlBase_c::testFlag(int) */
#define A_CLEAR_FLAG       0x80056CB0u   /* daPlBase_c::clearFlag(int) */
#define A_PROC_SETUP       0x80164FD0u   /* m3d::proc_c::setup(allocator (0: m3d's), u32 *size) */
#define A_LEAF_ENTRY       0x8016A0E0u   /* m3d::scnLeaf_c::entry() - draw this frame */
#define A_LEAF_FREE        0x8016A090u   /* m3d::scnLeaf_c::remove() */
#define A_LEAF_GETTYPE     0x8000F720u
#define A_GET_CAMERA_BY_ID 0x80164B20u   /* GetCameraByID(id) -> nw4r::g3d::CameraData* */
#define A_GET_CUR_CAMERA   0x80164B40u   /* GetCurrentCameraID() */
#define A_GET_CAMERA_MTX   0x802540B0u   /* nw4r::g3d::Camera::GetCameraMtx(MTX34*) */
#define A_G3D_INVALIDATE   0x8024D5E0u   /* nw4r::g3d::G3DState::Invalidate(flags) */
#define A_DISABLE_IND_TEX  0x80164F50u   /* m3d::DisableIndirectTexturing() */
#define G_GAME_MODE        0x8042A204u   /* 2: the title screen */
/* GX */
#define A_GXClearVtxDesc     0x801C4070u
#define A_GXSetVtxDesc       0x801C37C0u
#define A_GXSetVtxAttrFmt    0x801C40B0u
#define A_GXInitTexObj       0x801C6D90u
#define A_GXInitTexObjLOD    0x801C6FF0u
#define A_GXLoadTexObj       0x801C74C0u
#define A_GXSetNumChans      0x801C6AC0u
#define A_GXSetNumTexGens    0x801C4A20u
#define A_GXSetTexCoordGen2  0x801C47D0u
#define A_GXSetNumIndStages  0x801C8110u
#define A_GXSetTevDirect     0x801C8130u
#define A_GXSetNumTevStages  0x801C88F0u
#define A_GXSetTevOrder      0x801C8790u
#define A_GXSetTevOp         0x801C8250u
#define A_GXSetAlphaCompare  0x801C86C0u
#define A_GXSetBlendMode     0x801C8DC0u
#define A_GXSetZMode         0x801C8E70u
#define A_GXSetZCompLoc      0x801C8EB0u
#define A_GXSetCullMode      0x801C5860u
#define A_GXSetColorUpdate   0x801C8E10u
#define A_GXLoadPosMtxImm    0x801C9940u
#define A_GXSetCurrentMtx    0x801C9A60u
#define A_GXBegin            0x801C5570u

/* ---- player fields ---- */
#define PL_POS          0x0AC       /* mVec3_c mPos */
#define PL_SPEED        0x0E8       /* mVec3_c mSpeed */
#define PL_SPEED_F      0x10C       /* float mSpeedF */
#define PL_MAX_SPEED_F  0x110       /* float mMaxSpeedF (the ground step chases mSpeedF to it) */
#define PL_ACCEL_F      0x11C       /* float: how fast it does */
#define PL_DIRECTION    0x348       /* u8 mDirection: 0 right, 1 left */
#define PL_PLR_NO       0x38D       /* s8 mPlrNo */
#define PL_KEY          0xEA4       /* dAcPyKey_c mKey (+4 mDownButtons) */
#define PL_MODE         0x1090      /* PLAYER_MODE_e mPlayerMode */
#define PL_BG_CROSS1    0x10D4      /* u32 mNowBgCross1 */
#define PL_DEMO_MGR     0x1418      /* demo (cutscene) state manager */
#define PL_IS_DEMO      0x1460      /* bool m_isDemoMode */
#define PL_STATE_MGR    0x1464      /* state manager */
#define PL_TYPE         0x14D4      /* PLAYER_TYPE_e mPlayerType (dAcPy_c) */
enum { BGC_FOOT = 1 << 0, BGC_HEAD = 1 << 1, BGC_WALL = 1 << 2, BGC_WALL_TOUCH_L = 1 << 3, BGC_WALL_TOUCH_R = 1 << 4,
       BGC_WATER_SUBMERGED = 1 << 16 };
enum { KEY_DOWN = 1 << 0, KEY_UP = 1 << 1, KEY_RIGHT = 1 << 2, KEY_LEFT = 1 << 3, KEY_TWO = 1 << 8, KEY_ONE = 1 << 9,
       KEY_B = 1 << 10, KEY_A = 1 << 11 };
enum { MODE_SMALL, MODE_MUSHROOM, MODE_FIRE, MODE_MINI, MODE_PROPELLER, MODE_PENGUIN, MODE_ICE };
#define FLAG_NO_DRAW    188
#define VT_DACPY        0x80325420u /* dAcPy_c's vtable (vptr at +0x60) */         /* daPlBase_c flag: skip drawing this frame (damage blink) */

/* ---- our guest memory (past the module, below the renderer's texture limit) ---- */
#define RETRO_MEM       0x81900000u
#define RM_PROC         (RETRO_MEM + 0x0000)   /* 8 x 0x10: one draw proc per player */
#define RM_VTABLE       (RETRO_MEM + 0x0100)   /* the procs' vtable */
#define RM_CAMERA       (RETRO_MEM + 0x0200)   /* nw4r::g3d::Camera {CameraData*} */
#define RM_MTX          (RETRO_MEM + 0x0220)   /* MTX34 */
#define RM_VEC          (RETRO_MEM + 0x0260)   /* mVec3_c for posMove */
#define RM_TEXOBJ       (RETRO_MEM + 0x0300)   /* GXTexObj per sheet (0x20 each) */
#define RM_TEX          (RETRO_MEM + 0x1000)   /* texture data */
#define RM_TEX_END      (RETRO_MEM + 0x100000)
/* fake guest addresses of native functions (rt_register_native) */
#define N_PROC_DTOR     0x80003F00u
#define N_PROC_DRAW_OPA 0x80003F04u
#define N_PROC_DRAW_XLU 0x80003F08u

#define gcall rt_gcall
#define VFN(obj, off) R32(R32(obj) + (off))
static float RF(uint32_t a) { uint32_t u = R32(a); float f; memcpy(&f, &u, 4); return f; }
static void WF(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); W32(a, u); }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ================================================================== sprite sheets */
enum { FR_IDLE, FR_WALK1, FR_WALK2, FR_WALK3, FR_SKID, FR_JUMP, FR_FALL,
       FR_SWIM1, FR_SWIM2, FR_SWIM3, FR_SWIM4, FR_SWIM5, FR_SWIM6, FR_SWIM7, FR_SWIM8,
       FR_CLIMB1, FR_CLIMB2, FR_HANG1, FR_HANG2, FR_FLAGPOLE1, FR_FLAGPOLE2, FR_FRONT, FR_BACK,
       FR_VICTORY, FR_DEATH, FR_CROUCH, FR_GROW, FR_SHOOT, FR_COUNT };
static const char *frame_names[FR_COUNT] = {
    "idle", "walk1", "walk2", "walk3", "skid", "jump", "fall",
    "swim1", "swim2", "swim3", "swim4", "swim5", "swim6", "swim7", "swim8",
    "climb1", "climb2", "hang1", "hang2", "flagpole1", "flagpole2", "front", "back",
    "victory", "death", "crouch", "grow", "shoot" };
static const int frame_fallback[FR_COUNT] = {     /* used when a sheet doesn't have the frame */
    [FR_IDLE] = -1, [FR_WALK1] = FR_IDLE, [FR_WALK2] = FR_WALK1, [FR_WALK3] = FR_WALK1, [FR_SKID] = FR_IDLE,
    [FR_JUMP] = FR_IDLE, [FR_FALL] = FR_JUMP, [FR_SWIM1] = FR_JUMP, [FR_SWIM2] = FR_SWIM1, [FR_SWIM3] = FR_SWIM1,
    [FR_SWIM4] = FR_SWIM3, [FR_SWIM5] = FR_SWIM3, [FR_SWIM6] = FR_SWIM5, [FR_SWIM7] = FR_SWIM1, [FR_SWIM8] = FR_SWIM3,
    [FR_CLIMB1] = FR_IDLE, [FR_CLIMB2] = FR_CLIMB1, [FR_HANG1] = FR_CLIMB1, [FR_HANG2] = FR_CLIMB2,
    [FR_FLAGPOLE1] = FR_CLIMB1, [FR_FLAGPOLE2] = FR_CLIMB2, [FR_FRONT] = FR_IDLE, [FR_BACK] = FR_IDLE,
    [FR_VICTORY] = FR_IDLE, [FR_DEATH] = FR_JUMP, [FR_CROUCH] = FR_IDLE, [FR_GROW] = FR_IDLE, [FR_SHOOT] = FR_WALK2 };
enum { SH_SMALL, SH_SUPER, SH_FIRE, SH_ICE, SH_PROPELLER, SH_PENGUIN, SH_MINI, SH_COUNT };
static const char *sheet_names[SH_COUNT] = {"small", "super", "fire", "ice", "propeller", "penguin", "mini"};
typedef struct {
    int state;                       /* 0 not tried, 1 ready, -1 missing / still a template */
    int w, h, tw, th;                /* picture size, texture size (multiple of 4) */
    int bw, bh;                      /* frame box */
    int fr[FR_COUNT][2];             /* frame top-left, -1 = not in this sheet */
    uint32_t tex;                    /* guest address of the RGBA8 texture */
} sheet_t;
static sheet_t sheets[SH_COUNT];
static uint32_t tex_next = RM_TEX;

/* frames.json: {"sheets": {"small": {"file": .., "box": [w, h], "frames": {"idle": [x, y, w, h], ...}}}} */
static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *d = malloc(n + 1); if (fread(d, 1, n, f) != (size_t)n) n = 0; d[n] = 0; fclose(f); return d;
}
/* the end of the JSON value starting at p (object, array, string or scalar) */
static const char *json_skip(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p == '"') { for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++; return *p ? p + 1 : p; }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') { p = json_skip(p) - 1; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}
/* the value of "key" directly inside the object [obj, end) */
static const char *json_get(const char *obj, const char *end, const char *key) {
    const char *p = obj; while (p < end && *p != '{') p++;
    if (p >= end) return NULL;
    p++;
    size_t kl = strlen(key);
    while (p < end) {
        while (p < end && *p != '"' && *p != '}') p++;
        if (p >= end || *p == '}') return NULL;
        const char *k = p + 1, *ke = json_skip(p) - 1;
        p = ke + 1; while (p < end && *p != ':') p++;
        p++; while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if ((size_t)(ke - k) == kl && !memcmp(k, key, kl)) return p;
        p = json_skip(p);
    }
    return NULL;
}
static int json_ints(const char *p, int *v, int n) {      /* [a, b, ...] */
    int i = 0; if (!p || *p != '[') return 0;
    p++;
    while (i < n) { char *e; long x = strtol(p, &e, 10); if (e == p) break; v[i++] = (int)x; p = e; while (*p == ' ' || *p == ',') p++; }
    return i;
}

static const char *retro_dir(void) { return "mario"; }
static void sheet_load(int s) {
    sheet_t *sh = &sheets[s];
    sh->state = -1;
    char path[1200]; snprintf(path, sizeof path, "%s/ui/retro/%s/frames.json", g_root, retro_dir());
    char *js = read_text(path);
    if (!js) { rt_log("retro", "no %s", path); return; }
    const char *end = js + strlen(js);
    const char *sheets_o = json_get(js, end, "sheets");
    const char *so = sheets_o ? json_get(sheets_o, json_skip(sheets_o), sheet_names[s]) : NULL;
    if (!so) { free(js); return; }
    const char *se = json_skip(so);
    int box[2] = {16, 16}; json_ints(json_get(so, se, "box"), box, 2);
    sh->bw = box[0]; sh->bh = box[1];
    const char *fo = json_get(so, se, "frames"), *fe = fo ? json_skip(fo) : NULL;
    int any = 0;
    for (int f = 0; f < FR_COUNT; f++) {
        int r[4]; sh->fr[f][0] = sh->fr[f][1] = -1;
        if (fo && json_ints(json_get(fo, fe, frame_names[f]), r, 4) >= 2) { sh->fr[f][0] = r[0]; sh->fr[f][1] = r[1]; any = 1; }
    }
    free(js);
    if (!any) return;
    snprintf(path, sizeof path, "%s/ui/retro/%s/%s.png", g_root, retro_dir(), sheet_names[s]);
    int w, h; uint32_t *px = ui_load_png(path, &w, &h);      /* 0xAARRGGBB */
    if (!px) { rt_log("retro", "no %s", path); return; }
    /* Each frame is copied into its own cell of the texture (cells of the frame box size, 16 per
       row). Frames painted closer together than the box (the small sheet's swim row is 14 pixels
       apart) would show a sliver of their neighbour: a run of columns that touches the edge of
       the box and goes on outside it is a neighbour's, unless it's the frame's biggest. A frame
       with nothing painted (or a template's white box) counts as missing. */
    int cells = 0; for (int f = 0; f < FR_COUNT; f++) if (sh->fr[f][0] >= 0) cells++;
    int bw = sh->bw, bh = sh->bh, per_row = 16;
    int tw = per_row * bw, th = ((cells + per_row - 1) / per_row) * bh;
    tw = (tw + 3) & ~3; th = (th + 3) & ~3;
    uint32_t *cell = calloc((size_t)tw * th, 4);
    #define PX(x, y) ((x) >= 0 && (y) >= 0 && (x) < w && (y) < h ? px[(y) * w + (x)] : 0u)
    #define PAINTED(c) (((c) >> 24) && ((c) & 0xFFFFFF) != 0xFFFFFF)
    int k = 0, any_painted = 0;
    for (int f = 0; f < FR_COUNT; f++) {
        if (sh->fr[f][0] < 0) continue;
        int fx = sh->fr[f][0], fy = sh->fr[f][1];
        int occ[64] = {0}, painted = 0;
        for (int x = 0; x < bw && x < 64; x++) for (int y = 0; y < bh; y++) {
            uint32_t c = PX(fx + x, fy + y);
            if (c >> 24) occ[x] = 1;
            if (PAINTED(c)) painted = 1;
        }
        if (!painted) { sh->fr[f][0] = sh->fr[f][1] = -1; continue; }
        any_painted = 1;
        /* runs of occupied columns; the widest one is the frame's own */
        int keep[64]; for (int x = 0; x < 64; x++) keep[x] = 1;
        int runs[16][2], nr = 0, best = 0;
        for (int x = 0; x < bw && nr < 16; ) {
            if (!occ[x]) { x++; continue; }
            int x0 = x; while (x < bw && occ[x]) x++;
            runs[nr][0] = x0; runs[nr][1] = x; if (x - x0 > runs[best][1] - runs[best][0] || !nr) best = nr; nr++;
        }
        for (int r = 0; r < nr; r++) {
            if (r == best) continue;
            int x0 = runs[r][0], x1 = runs[r][1], outside = 0;
            for (int y = 0; y < bh; y++) {
                if (x0 == 0 && (PX(fx - 1, fy + y) >> 24)) outside = 1;
                if (x1 == bw && (PX(fx + bw, fy + y) >> 24)) outside = 1;
            }
            if (outside) for (int x = x0; x < x1; x++) keep[x] = 0;
        }
        int cx = (k % per_row) * bw, cy = (k / per_row) * bh;
        for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
            uint32_t c = x < 64 && keep[x] ? PX(fx + x, fy + y) : 0;
            cell[(cy + y) * tw + cx + x] = c;
        }
        sh->fr[f][0] = cx; sh->fr[f][1] = cy;
        k++;
    }
    #undef PX
    #undef PAINTED
    free(px);
    if (!any_painted) { free(cell); rt_log("retro", "%s.png is still a template: using another sheet", sheet_names[s]); return; }
    {   const char *dd = getenv("RT_RETRO_DUMP_SHEETS");           /* testing: the cells as a .ppm (alpha as magenta) */
        if (dd) { char pp[1300]; snprintf(pp, sizeof pp, "%s/cells_%s.ppm", dd, sheet_names[s]); FILE *o = fopen(pp, "wb");
            if (o) { fprintf(o, "P6\n%d %d\n255\n", tw, th); for (int i = 0; i < tw * th; i++) { uint32_t c = cell[i];
                uint8_t px3[3] = {(c >> 24) ? (uint8_t)(c >> 16) : 255, (c >> 24) ? (uint8_t)(c >> 8) : 0, (c >> 24) ? (uint8_t)c : 255}; fwrite(px3, 1, 3, o); } fclose(o); } } }
    /* GX RGBA8: 4x4 tiles of 64 bytes, first the 16 AR pairs then the 16 GB pairs */
    uint32_t bytes = (uint32_t)tw * th * 4;
    if (tex_next + bytes > RM_TEX_END) { free(cell); rt_log("retro", "out of texture memory"); return; }
    uint8_t *dst = G(tex_next);
    for (int ty = 0; ty < th; ty += 4) for (int tx = 0; tx < tw; tx += 4) {
        for (int i = 0; i < 16; i++) {
            int x = tx + (i & 3), y = ty + (i >> 2);
            uint32_t c = cell[y * tw + x];
            if (!(c >> 24)) c = 0;                           /* fully transparent: black (no fringes) */
            dst[i * 2] = (uint8_t)(c >> 24); dst[i * 2 + 1] = (uint8_t)(c >> 16);
            dst[32 + i * 2] = (uint8_t)(c >> 8); dst[32 + i * 2 + 1] = (uint8_t)c;
        }
        dst += 64;
    }
    free(cell);
    sh->w = tw; sh->h = th; sh->tw = tw; sh->th = th; sh->tex = tex_next;
    tex_next = (tex_next + bytes + 31) & ~31u;
    sh->state = 1;
    rt_log("retro", "%s/%s.png: %dx%d, %d frames of %dx%d", retro_dir(), sheet_names[s], w, h, k, sh->bw, sh->bh);
}
static sheet_t *sheet_for(int mode) {
    int s = mode == MODE_MUSHROOM ? SH_SUPER : mode == MODE_FIRE ? SH_FIRE : mode == MODE_ICE ? SH_ICE :
            mode == MODE_PROPELLER ? SH_PROPELLER : mode == MODE_PENGUIN ? SH_PENGUIN : mode == MODE_MINI ? SH_MINI : SH_SMALL;
    { static int all; if (!all && getenv("RT_RETRO_DUMP_SHEETS")) { all = 1; for (int i = 0; i < SH_COUNT; i++) if (!sheets[i].state) sheet_load(i); } }
    if (!sheets[s].state) sheet_load(s);
    if (sheets[s].state > 0) return &sheets[s];
    if (s == SH_FIRE || s == SH_ICE || s == SH_PROPELLER || s == SH_PENGUIN) {   /* big forms: the super sheet */
        if (!sheets[SH_SUPER].state) sheet_load(SH_SUPER);
        if (sheets[SH_SUPER].state > 0) return &sheets[SH_SUPER];
    }
    if (!sheets[SH_SMALL].state) sheet_load(SH_SMALL);
    return sheets[SH_SMALL].state > 0 ? &sheets[SH_SMALL] : NULL;
}

/* ================================================================== per player */
typedef struct {
    uint32_t actor, uid;             /* the dAcPy_c this is for (0: none), its fBase_c unique ID */
    smb1_player p;
    int controlled;                  /* moved by smb1 this frame */
    int was_controlled;
    int in_execute;                  /* inside its executeMain (setJump calls are its own) */
    int ext_jump; float ext_jump_speed;
    float move_x0, move_dx;          /* this frame's move: x before, how far smb1 asked */
    int ground_step;                 /* moved by NSMBW's ground step this frame (slopes: less x) */
    int frame, facing_left;          /* picture */
    int anim_t;                      /* (NSMBW-moved states) walk cycle */
    uint32_t proc;                   /* its m3d::proc_c */
    int draw_pending, draw_on_top;   /* draw_on_top: in the translucent pass, over everything (goal) */
    int swim_pic, swim_last_anim;    /* swimming: the sheet's 4 pictures, stepped with SMB1's animation */
    struct {                         /* the goal pole, SMB1 style (goal_step) */
        int phase, t, slide_frames, hidden;
        float pole_x, castle_x, x, y, end_y;
        uint8_t dummy;
        smb1_player p;
    } goal;
    struct { int on, side, timer; float cx; int anim_t, anim; } vine;   /* on a vine: left (0) / right (1) of it */
    int bump_t, bump_sound;          /* head hit a block: frames left to hear what it did; what it did */
    uint64_t obj_sound_frame;        /* a coin / item / brick sound of its own this frame (a block it hit) */
    int was_dead, last_mode, age;
} retro_t;
static retro_t R[8];
static ppc_context *draw_ctx;

/* is this player a retro character? Its player slot picked Retro Mario (SMB1) in the character
   select (rt_chars.c); or, for testing, settings.ini retro_mario = 1 makes every Mario one. */
static retro_t *retro_of(uint32_t pl) {
    if (!pl) return NULL;
    if (R32(pl + 0x60) != VT_DACPY) return NULL;                        /* a player (not Yoshi) */
    if (R32(G_GAME_MODE) == 2) return NULL;                             /* (title screen: 3D characters) */
    int n = (int8_t)R8(pl + PL_PLR_NO);
    if (n < 0 || n >= 8) return NULL;
    if (!chars_is_smb1(n) && !(rt_setting("retro_mario", 0) && R32(pl + PL_TYPE) == 0)) return NULL;
    retro_t *r = &R[n];
    if (r->actor != pl || r->uid != R32(pl)) {
        /* a new player actor (new level / area: the old draw proc went with the scene's heap) */
        memset(r, 0, sizeof *r);
        r->actor = pl; r->uid = R32(pl);
        int mode = (int)R32(pl + PL_MODE);
        r->last_mode = mode;
        smb1_init(&r->p, mode == MODE_SMALL || mode == MODE_MINI, R8(pl + PL_DIRECTION) ? SMB1_FACE_LEFT : SMB1_FACE_RIGHT);
        rt_log("retro", "player %d (%08X) is SMB1 Mario", n, pl);
    }
    return r;
}

static const char *state_name(ppc_context *ctx, uint8_t *base, uint32_t mgr) {
    static char s[64];
    uint32_t st = gcall(ctx, base, VFN(mgr, 0x28), 1, mgr), np = st ? R32(st + 4) : 0;
    s[0] = 0;
    if (np < 0x80000000u || np >= 0x81900000u) return s;
    char full[128]; int i;
    for (i = 0; i < 127 && R8(np + i); i++) full[i] = (char)R8(np + i);
    full[i] = 0;
    const char *u = strstr(full, "StateID_");
    snprintf(s, sizeof s, "%s", u ? u + 8 : full);
    return s;
}
static int movement_state(const char *st) {
    static const char *list[] = {"Walk", "Jump", "Fall", "Land", "Crouch", "SitJump", "Turn", "Swim", NULL};
    for (int i = 0; list[i]; i++) if (!strcmp(st, list[i])) return 1;
    return 0;
}

/* ---- testing: RT_RETRO_SCRIPT="R:60 RB:30 RBA:20 :40 ..." presses buttons from the first frame
   the retro player can move (R/L/U/D directions, A jump (2), B run (1); n frames each), through
   the emulated remote like the keyboard. RT_RETRO_EXIT=n quits n frames after the script. */
uint32_t g_retro_test_buttons;
static void test_script_step(void) {
    static const char *spec; static int init, tok_left, done_frames; static char buf[1024], *save, *tok;
    if (!init) { init = 1; spec = getenv("RT_RETRO_SCRIPT"); if (spec) { snprintf(buf, sizeof buf, "%s", spec); tok = strtok_r(buf, " ", &save); tok_left = -1; } }
    if (!spec) return;
    while (tok && tok_left == 0) { tok = strtok_r(NULL, " ", &save); tok_left = -1; }
    if (!tok) {
        g_retro_test_buttons = 0;
        const char *ex = getenv("RT_RETRO_EXIT");
        if (ex && ++done_frames > atoi(ex)) { rt_log("retro", "test script done"); exit(0); }
        return;
    }
    if (tok_left < 0) { const char *c = strchr(tok, ':'); tok_left = c ? atoi(c + 1) : 1; }
    uint32_t b = 0;
    for (const char *k = tok; *k && *k != ':'; k++)
        b |= *k == 'R' ? 0x0004 : *k == 'L' ? 0x0008 : *k == 'U' ? 0x0002 : *k == 'D' ? 0x0001 : *k == 'A' ? 0x0100 : *k == 'B' ? 0x0200 : 0;
    g_retro_test_buttons = b;
    tok_left--;
}

/* ================================================================== movement */
static uint8_t smb1_buttons(uint32_t pl) {
    uint16_t k = R16(pl + PL_KEY + 4);                                  /* mDownButtons */
    uint8_t b = 0;
    if (k & (KEY_TWO | KEY_A)) b |= SMB1_A;
    if (k & (KEY_ONE | KEY_B)) b |= SMB1_B;
    if (k & KEY_UP) b |= SMB1_UP;
    if (k & KEY_DOWN) b |= SMB1_DOWN;
    if ((k & KEY_LEFT) && !(k & KEY_RIGHT)) b |= SMB1_LEFT;
    if ((k & KEY_RIGHT) && !(k & KEY_LEFT)) b |= SMB1_RIGHT;
    return b;
}

/* daPlBase_c's per-frame speed and position step (called from executeMain between the state's
   execute and the collision check). */
void hle_fn_80056E40(ppc_context *ctx, uint8_t *base) {
    uint32_t pl = ctx->r[3];
    retro_t *r = retro_of(pl);
    if (r) r->controlled = 0;
    if (!r || R8(pl + PL_IS_DEMO) || !movement_state(state_name(ctx, base, pl + PL_STATE_MGR))) {
        orig_fn_80056E40(ctx, base);
        return;
    }
    smb1_player *p = &r->p;
    int mode = (int)R32(pl + PL_MODE);
    p->PlayerSize = (mode == MODE_SMALL || mode == MODE_MINI) ? 1 : 0;
    uint32_t bg = R32(pl + PL_BG_CROSS1);
    const char *st = state_name(ctx, base, pl + PL_STATE_MGR);
    p->SwimmingFlag = !strcmp(st, "Swim");
    p->WaterTop = p->SwimmingFlag && !(bg & BGC_WATER_SUBMERGED);
    if (!r->was_controlled) {
        /* taking over from NSMBW's movement: carry the speed over */
        float sf = RF(pl + PL_SPEED_F);
        int xs = (int)lrintf(sf * 16.0f); if (xs > 0x28) xs = 0x28; if (xs < -0x28) xs = -0x28;
        p->Player_X_Speed = (uint8_t)xs; p->Player_X_MoveForce = 0;
        p->Player_XSpeedAbsolute = (uint8_t)(xs < 0 ? -xs : xs);
        p->PlayerFacingDir = R8(pl + PL_DIRECTION) ? SMB1_FACE_LEFT : SMB1_FACE_RIGHT;
        if (xs) p->Player_MovingDir = xs < 0 ? SMB1_FACE_LEFT : SMB1_FACE_RIGHT;
        float vy = RF(pl + PL_SPEED + 4);
        p->Player_Y_Speed = (uint8_t)(int8_t)(vy > 4 ? -4 : vy < -4 ? 4 : -(int)lrintf(vy));
        p->Player_Y_MoveForce = 0;
        p->Player_State = (bg & BGC_FOOT) ? SMB1_ON_GROUND : p->SwimmingFlag ? SMB1_JUMP_SWIM : SMB1_FALLING;
    }
    if (r->ext_jump) {                                                  /* bounced by an enemy / spring */
        if (r->ext_jump_speed > 5.0f) smb1_spring(p, (smb1_buttons(pl) & SMB1_A) != 0);
        else { smb1_stomp(p); if (p->Player_State == SMB1_ON_GROUND) p->Player_State = SMB1_FALLING; sfx_play(retro_dir(), "stompswim"); }
        r->ext_jump = 0;
    }
    smb1_motion m;
    int st0 = p->Player_State; int8_t ys0 = (int8_t)p->Player_Y_Speed; uint8_t ymf0 = p->Player_Y_MoveForce;
    smb1_frame(p, smb1_buttons(pl), &m);
    if (m.jumped) sfx_play(retro_dir(), p->SwimmingFlag ? "stompswim" : p->PlayerSize ? "jumpsmall" : "jump");
    /* SMB1 moves in whole pixels (1, 2, 1, 2... at 1.5 px a frame), which judders against NSMBW's
       smooth camera: the player moves by the speed instead - the same distance on average */
    float vx = p->JumpspringAnimCtrl ? 0.0f : (int8_t)p->Player_X_Speed / 16.0f;
    r->move_x0 = RF(pl + PL_POS); r->move_dx = vx;
    if (p->Player_State == SMB1_ON_GROUND && st0 == SMB1_ON_GROUND && !m.jumped && !p->SwimmingFlag) {
        /* on the ground NSMBW's own step moves the player at the SMB1 speed: it follows slopes
           and moving or tilting floors the way the other characters do */
        WF(pl + PL_SPEED_F, vx); WF(pl + PL_MAX_SPEED_F, vx); WF(pl + PL_ACCEL_F, 1000.0f);
        orig_fn_80056E40(ctx, base);
        r->ground_step = 1;
    } else {
        float dy;
        if (st0 != SMB1_ON_GROUND && p->Player_State != SMB1_ON_GROUND && !m.jumped && p->Player_State != SMB1_CLIMBING)
            dy = -((float)ys0 + ymf0 / 256.0f);                      /* (SMB1: y down; speed + fraction) */
        else dy = (float)-m.dy;
        WF(RM_VEC, vx); WF(RM_VEC + 4, dy); WF(RM_VEC + 8, 0.0f);
        gcall(ctx, base, A_POS_MOVE_DELTA, 2, pl, RM_VEC);
        WF(pl + PL_SPEED, vx); WF(pl + PL_SPEED + 4, dy); WF(pl + PL_SPEED_F, vx);
        r->ground_step = 0;
    }
    W8(pl + PL_DIRECTION, p->PlayerFacingDir == SMB1_FACE_LEFT ? 1 : 0);
    r->controlled = 1;
}

/* ---- the goal pole, the SMB1 way (FlagpoleSlide / PlayerEndLevel) ----
 * NSMBW's own goal sequence goes on as usual (it drives the course clear); what is drawn is SMB1
 * Mario: he slides down the pole at SMB1's climbing speed (the 1/256 accumulator gets $FF a frame:
 * 1 pixel a frame), waits for the flag (SMB1's flag comes down from the top at 2 pixels a frame:
 * 61 frames), swings round to the other side of the pole (ClimbAdderLow +14, facing left), lets
 * go after ClimbSideTimer ($18 frames), and walks right with SMB1's walking (AutoControlPlayer
 * with right) until he reaches the castle, where he's gone. He is drawn over everything meanwhile
 * (over the pole, in front of the other players doing their poses). */
#define A_CHECK_GROUND 0x800757B0u      /* dBc_c::checkGround(const mVec3_c *, float *out, u8 layer, u8, s8) */
#define PL_LAYER       0x38F
#define PL_CASTLE_X    0x3FC             /* mWarpPos.x: the goal castle (setDemoGoal) */
enum { G_NONE, G_SLIDE, G_WAIT, G_FLIP, G_WALK, G_GONE };
static float ground_below(ppc_context *ctx, uint8_t *base, uint32_t pl, float x, float y) {
    WF(RM_VEC, x); WF(RM_VEC + 4, y); WF(RM_VEC + 8, RF(pl + PL_POS + 8)); WF(RM_VEC + 12, y - 1000.0f);
    if (!gcall(ctx, base, A_CHECK_GROUND, 5, RM_VEC, RM_VEC + 12, R8(pl + PL_LAYER), 1, (uint32_t)-1)) return y - 1000.0f;
    return RF(RM_VEC + 12);
}
/* dAcPy_c::setDemoGoal(landPos, castleX, type): the player reached the goal pole */
void hle_fn_8013FCE0(ppc_context *ctx, uint8_t *base) {
    uint32_t pl = ctx->r[3]; retro_t *r = retro_of(pl);
    float y0 = r ? RF(pl + PL_POS + 4) : 0, castle = (float)ctx->f[1].d;
    orig_fn_8013FCE0(ctx, base);
    if (!r || !R8(pl + PL_IS_DEMO)) return;
    memset(&r->goal, 0, sizeof r->goal);
    r->goal.phase = G_SLIDE;
    r->goal.pole_x = RF(pl + PL_POS);                                   /* (NSMBW's player is put on the pole's middle) */
    r->goal.x = r->goal.pole_x - 7.0f;                                  /* SMB1: left of the pole, facing it */
    r->goal.y = y0;
    r->goal.castle_x = castle;
    r->goal.end_y = ground_below(ctx, base, pl, r->goal.pole_x, y0) + 2.0f;
    if (r->goal.end_y > y0) r->goal.end_y = y0;
    rt_log("retro", "goal: pole %.1f, grabbed at %.1f, slides to %.1f, castle %.1f (frame %llu)", r->goal.pole_x, y0, r->goal.end_y, castle, (unsigned long long)g_frames_presented);
}
static void goal_step(ppc_context *ctx, uint8_t *base, retro_t *r, uint32_t pl) {
    typeof(r->goal) *g = &r->goal;
    g->t++;
    switch (g->phase) {
    case G_SLIDE: {                                                     /* ClimbingSub, down held */
        int carry = g->dummy + 0xFF > 0xFF; g->dummy = (uint8_t)(g->dummy + 0xFF);
        if (carry) g->y -= 1.0f;
        g->slide_frames++;
        if (g->y <= g->end_y) { g->y = g->end_y; g->phase = G_WAIT; g->t = 0; }
        r->frame = (g->slide_frames / 8) & 1 ? FR_FLAGPOLE2 : FR_FLAGPOLE1;   /* (climbing down: a new picture every 8 frames) */
        r->facing_left = 0;
        break;
    }
    case G_WAIT:                                                        /* the flag isn't down yet */
        r->frame = FR_FLAGPOLE1;
        if (g->t >= 61 - g->slide_frames) { g->phase = G_FLIP; g->t = 0; g->x += 14.0f; }
        break;
    case G_FLIP:                                                        /* the other side, facing left */
        r->frame = FR_FLAGPOLE1; r->facing_left = 1;
        if (g->t >= 0x18) {
            g->phase = G_WALK; g->t = 0; g->x += 4.0f;                  /* lets go */
            smb1_init(&g->p, r->p.PlayerSize, SMB1_FACE_RIGHT);
            g->p.PlayerSize = R32(pl + PL_MODE) == MODE_SMALL || R32(pl + PL_MODE) == MODE_MINI;
            g->p.Player_State = SMB1_FALLING;
        }
        break;
    case G_WALK: {                                                      /* AutoControlPlayer: right */
        smb1_motion m; smb1_frame(&g->p, SMB1_RIGHT, &m);
        g->x += (float)m.dx; g->y -= (float)m.dy;
        float gy = ground_below(ctx, base, pl, g->x, g->y + 8.0f);
        if (g->p.Player_State != SMB1_ON_GROUND && g->y <= gy) { g->y = gy; smb1_ground(&g->p, 1); }
        else if (g->p.Player_State == SMB1_ON_GROUND && gy < g->y - 1.0f) smb1_ground(&g->p, 0);
        else if (g->p.Player_State == SMB1_ON_GROUND) g->y = gy;
        smb1_gfx gf; smb1_graphics(&g->p, &gf);
        static const int walk[3] = {FR_WALK2, FR_WALK1, FR_WALK3};
        r->frame = gf.gfx == SMB1_GFX_JUMP || gf.gfx == SMB1_GFX_WALK && g->p.Player_State == SMB1_FALLING ? walk[gf.anim > 2 ? 2 : gf.anim]
                 : gf.gfx == SMB1_GFX_STAND ? FR_IDLE : walk[gf.anim > 2 ? 2 : gf.anim];
        r->facing_left = 0;
        if (g->x >= g->castle_x) { g->phase = G_GONE; rt_log("retro", "goal: into the castle"); }
        break;
    }
    case G_GONE: break;
    }
    g->hidden = g->phase == G_GONE;
    r->draw_on_top = 1;
}

/* ---- vines: SMB1 holds a vine on its left or its right side, nothing in between ----
 * Left/right don't move NSMBW's player on a vine; they swing him round to that side (ClimbingSub:
 * once per $18 frames). He is held 7 units left or right of the vine's middle (the middle of the
 * tile he caught it in), facing it. Climbing nets and rock walls stay NSMBW's. */
#define VINE_HOLD 7.0f
#define PL_FENCE_TYPE 0x2B8         /* mBc.mFenceType: 0 ivy (vines, beanstalks), 1 net, 2 rock wall */
static void vine_before(retro_t *r, uint32_t pl, const char *st) {
    int on = !strcmp(st, "Vine") && R32(pl + PL_FENCE_TYPE) == 0;   /* (only vines: nets and walls as NSMBW's) */
    if (!on) { r->vine.on = 0; return; }
    uint16_t k = R16(pl + PL_KEY + 4), kt = R16(pl + PL_KEY + 6);
    if (!r->vine.on) {
        r->vine.on = 1; r->vine.timer = 0;
        float x = RF(pl + PL_POS);
        r->vine.cx = floorf(x / 16.0f) * 16.0f + 8.0f;
        r->vine.side = x >= r->vine.cx;
    }
    if (r->vine.timer) r->vine.timer--;
    int want = (k & KEY_LEFT) && !(k & KEY_RIGHT) ? 0 : (k & KEY_RIGHT) && !(k & KEY_LEFT) ? 1 : -1;
    if (want >= 0 && want != r->vine.side && !r->vine.timer) { r->vine.side = want; r->vine.timer = 0x18; }
    if (!(k & (KEY_LEFT | KEY_RIGHT))) r->vine.timer = 0;            /* (InitCSTimer) */
    W16(pl + PL_KEY + 4, k & ~(KEY_LEFT | KEY_RIGHT));                  /* NSMBW doesn't move him sideways */
    W16(pl + PL_KEY + 6, kt & ~(KEY_LEFT | KEY_RIGHT));
}
static void vine_after(ppc_context *ctx, uint8_t *base, retro_t *r, uint32_t pl) {
    if (!r->vine.on || strcmp(state_name(ctx, base, pl + PL_STATE_MGR), "Vine")) { r->vine.on = 0; return; }
    WF(pl + PL_POS, r->vine.cx + (r->vine.side ? VINE_HOLD : -VINE_HOLD));
    W8(pl + PL_DIRECTION, r->vine.side ? 1 : 0);                      /* (facing the vine) */
    float vy = RF(pl + PL_SPEED + 4);
    if (fabsf(vy) > 0.01f) {                                            /* climbing: up 4 frames a picture, down 8 */
        if (++r->vine.anim_t >= (vy > 0 ? 4 : 8)) { r->vine.anim_t = 0; r->vine.anim ^= 1; }
    }
}

/* effects made while a retro player runs its own code are its own: none (rt: fx_block) */
static int fx_block;
static retro_t *acting;              /* the retro player whose update is running (sounds made now are its) */

/* dAcPy_c::executeMain: after it, the collision results go to the SMB1 code */
void hle_fn_801438A0(ppc_context *ctx, uint8_t *base) {
    uint32_t pl = ctx->r[3];
    retro_t *r = retro_of(pl);
    if (!r) { orig_fn_801438A0(ctx, base); return; }
    r->controlled = 0;                                                  /* (set by the move step, if it runs) */
    r->in_execute = 1;
    vine_before(r, pl, state_name(ctx, base, pl + PL_STATE_MGR));
    fx_block++; retro_t *was_acting = acting; acting = r;
    orig_fn_801438A0(ctx, base);
    fx_block--; acting = was_acting;
    r->in_execute = 0;
    vine_after(ctx, base, r, pl);
    r->age++;
    uint32_t bg = R32(pl + PL_BG_CROSS1);
    if (r->controlled) {
        smb1_player *p = &r->p;
        smb1_ground(p, (bg & BGC_FOOT) != 0);
        if ((bg & BGC_HEAD) && !r->bump_t && (int8_t)p->Player_Y_Speed <= 0) {   /* (what it hit: retro_sound) */
            r->bump_t = 6; r->bump_sound = r->obj_sound_frame == g_frames_presented;   /* (already heard: a coin from the block) */
        }
        if (bg & BGC_HEAD) smb1_head_bump(p, 1);
        /* a wall: the collision moved the player back from where SMB1 sent it (the BgCross wall
           bits don't say which side reliably) */
        float moved = RF(pl + PL_POS) - r->move_x0, need = r->ground_step ? 0.5f : 1.0f;   /* (slopes: cos) */
        if (r->move_dx > 0 && moved < r->move_dx * need - 0.01f) smb1_wall(p, SMB1_FACE_RIGHT);
        if (r->move_dx < 0 && moved > r->move_dx * need + 0.01f) smb1_wall(p, SMB1_FACE_LEFT);
    }
    if (r->bump_t && !--r->bump_t && !r->bump_sound) sfx_play(retro_dir(), "bump");   /* (nothing came of it: a bump) */
    /* the goal pole */
    if (r->goal.phase && !(R8(pl + PL_IS_DEMO) && !strcmp(state_name(ctx, base, pl + PL_DEMO_MGR), "DemoGoal"))) { r->goal.phase = G_NONE; r->draw_on_top = 0; }
    if (r->goal.phase) goal_step(ctx, base, r, pl);
    /* death, power-ups and power-downs */
    const char *dst = R8(pl + PL_IS_DEMO) ? state_name(ctx, base, pl + PL_DEMO_MGR) : "";
    {   static char last_demo[8][32]; int n = (int)(r - R);             /* into a pipe */
        if (!strncmp(dst, "DemoInDokan", 11) && strncmp(last_demo[n], "DemoInDokan", 11) && r->age > 1) sfx_play(retro_dir(), "pipepowerdown");
        snprintf(last_demo[n], sizeof last_demo[n], "%s", dst); }
    int dead = strstr(dst, "Down") || strstr(dst, "Dead") || strstr(dst, "EatDie") || strstr(dst, "FallDown") || strstr(dst, "FireDown");
    if (dead && !r->was_dead) { sfx_stop_all(); sfx_play(retro_dir(), "death"); }
    r->was_dead = dead;
    int mode = (int)R32(pl + PL_MODE);
    if (mode != r->last_mode && !dead && !R8(pl + PL_IS_DEMO) && r->was_controlled) {
        if (mode == MODE_SMALL || (mode == MODE_MUSHROOM && r->last_mode != MODE_SMALL && r->last_mode != MODE_MINI)) sfx_play(retro_dir(), "pipepowerdown");
        else sfx_play(retro_dir(), "powerup");
    }
    r->last_mode = mode;
    r->was_controlled = r->controlled;
    {   static int started; if (r->controlled) started = 1; if (started) test_script_step(); }
    {   static int star = -1; if (star < 0) star = getenv("RT_RETRO_STAR") != NULL;      /* testing: invincible */
        if (star) W32(pl + 0x1070, 600); }
    {   static int lg = -1; if (lg < 0) lg = getenv("RT_RETROLOG") != NULL;     /* testing: a line per frame */
        if (lg) rt_log("retro", "%s %s%s%s pos %.1f,%.1f bg %08X smb1 st %d xs %02X ys %02X face %d keys %04X goal %d %.1f,%.1f",
                       r->controlled ? "smb1" : "nsmbw", state_name(ctx, base, pl + PL_STATE_MGR), R8(pl + PL_IS_DEMO) ? "/" : "", dst, RF(pl + PL_POS), RF(pl + PL_POS + 4),
                       R32(pl + PL_BG_CROSS1), r->p.Player_State, r->p.Player_X_Speed, r->p.Player_Y_Speed, r->p.PlayerFacingDir, R16(pl + PL_KEY + 4),
                       r->goal.phase, r->goal.x, r->goal.y); }
}

/* dAcPy_c::setJump(speedY f1, speedF f2, allowSteer, keyMode, jumpMode): from outside the player's
   own update it is a bounce (stomping an enemy, a spring, another player's head) */
void hle_dAcPy_vf3FC(ppc_context *ctx, uint8_t *base) {
    retro_t *r = retro_of(ctx->r[3]);
    if (r && !r->in_execute) { r->ext_jump = 1; r->ext_jump_speed = (float)ctx->f[1].d; }
    orig_dAcPy_vf3FC(ctx, base);
}
/* moves SMB1 doesn't have */
void hle_fn_801285D0(ppc_context *ctx, uint8_t *base) {        /* dAcPy_c: start a wall slide? (1 = did) */
    if (retro_of(ctx->r[3])) { ctx->r[3] = 0; return; }
    orig_fn_801285D0(ctx, base);
}
void hle_fn_801266B0(ppc_context *ctx, uint8_t *base) {        /* dAcPy_c::setHipAttackAction */
    if (retro_of(ctx->r[3])) { ctx->r[3] = 0; return; }
    orig_fn_801266B0(ctx, base);
}
void hle_fn_8005E780(ppc_context *ctx, uint8_t *base) {        /* dAcPyKey_c::triggerShakeJump */
    uint32_t pl = ctx->r[3] - PL_KEY;
    if (pl >= 0x80000000u && R32(pl) && retro_of(pl) && R32(pl + PL_MODE) != MODE_PROPELLER) { ctx->r[3] = 0; return; }
    orig_fn_8005E780(ctx, base);
}

/* no particles: the dEf::createPlayerEffect family (first argument: the player number) */
static int retro_player_no(int n) {
    if (n < 0 || n >= 8) return 0;
    for (int i = 0; i < 8; i++) if (R[i].actor && (int8_t)R8(R[i].actor + PL_PLR_NO) == n && retro_of(R[i].actor)) return 1;
    return 0;
}
static void pe_log(ppc_context *ctx, int name_reg, int dropped) {
    static int lg = -1; if (lg < 0) lg = getenv("RT_EFFECT_LOG") != NULL;
    if (!lg) return;
    uint32_t np = ctx->r[name_reg]; char nm[48] = "?"; int i;
    if (np >= 0x80000000u && np < 0x81800000u) { for (i = 0; i < 47 && R8(np + i); i++) nm[i] = (char)R8(np + i); nm[i] = 0; }
    rt_log("effect", "player %d: %s%s (lr %08X)", (int)ctx->r[3], nm, dropped ? " - dropped (retro)" : "", ctx->lr);
}
#define NO_EFFECT(fn, name_reg) void hle_##fn(ppc_context *ctx, uint8_t *base) { \
    int drop = retro_player_no((int)ctx->r[3]); pe_log(ctx, name_reg, drop); \
    if (drop) { ctx->r[3] = 0; return; } orig_##fn(ctx, base); }
NO_EFFECT(createPlayerEffect__3dEfFiPCcUlPC7mVec3_cPC7mAng3_cPC7mVec3_c, 4)
NO_EFFECT(createPlayerEffect__3dEfFiPQ23mEf13levelEffect_cPCcUlPC7mVec3_cPC7mAng3_cPC7mVec3_c, 5)
NO_EFFECT(createPlayerEffect__3dEfFiPQ23dEf14followEffect_cPCcUlPC7mVec3_cPC7mAng3_cPC7mVec3_c, 5)
NO_EFFECT(createPlayerEffect_change__3dEfFiPCcUlPC7mVec3_cPC7mAng3_cPC7mVec3_c, 4)
NO_EFFECT(createPlayerEffect_change__3dEfFiPQ23mEf13levelEffect_cPCcUlPC7mVec3_cPC7mAng3_cPC7mVec3_c, 5)
NO_EFFECT(createPlayerEffect_change__3dEfFiPQ23dEf14followEffect_cPCcUlPC7mVec3_cPC7mAng3_cPC7mVec3_c, 5)

/* ---- what SMB1 Mario can't do ----
 * State changes go through the state manager's changeState (fn_80047430, r3 = the manager, r4 =
 * the new state's sStateID): for a retro player these don't happen - sliding down slopes (Slip),
 * crouching while small (Crouch, SitJump: only the big SMB1 Mario ducks), lifting up another
 * player (LiftUp), riding Yoshi (RideYoshi). */
void hle_fn_80047430(ppc_context *ctx, uint8_t *base) {
    uint32_t mgr = ctx->r[3], id = ctx->r[4];
    if (mgr > 0x80000000u + PL_STATE_MGR && id >= 0x80000000u) {
        uint32_t pl = mgr - PL_STATE_MGR;
        if (R32(pl + 0x60) == VT_DACPY && retro_of(pl)) {
            uint32_t np = R32(id + 4); char nm[48] = ""; int i;
            if (np >= 0x80000000u && np < 0x81800000u) { for (i = 0; i < 47 && R8(np + i); i++) nm[i] = (char)R8(np + i); nm[i] = 0; }
            const char *st = strstr(nm, "StateID_"); st = st ? st + 8 : nm;
            int mode = (int)R32(pl + PL_MODE), small = mode == MODE_SMALL || mode == MODE_MINI;
            if (!strcmp(st, "Slip") || !strcmp(st, "LiftUp") || !strcmp(st, "RideYoshi") ||
                (small && (!strcmp(st, "Crouch") || !strcmp(st, "SitJump")))) {
                static int lg = -1; if (lg < 0) lg = getenv("RT_RETROLOG") != NULL;
                if (lg) rt_log("retro", "no %s for SMB1 Mario", st);
                return;
            }
        }
    }
    orig_fn_80047430(ctx, base);
}
/* daYoshi_c: let a player get on (r4 = the player; 1 = got on) */
void hle_fn_8014eb70__9daYoshi_cFP7dAcPy_ci(ppc_context *ctx, uint8_t *base) {
    if (retro_of(ctx->r[4])) { ctx->r[3] = 0; return; }
    orig_fn_8014eb70__9daYoshi_cFP7dAcPy_ci(ctx, base);
}
/* dAcPy_c::spinLiftUp(actor, really): pick up / carry something (shells, blocks, players) */
void hle_fn_8012E400(ppc_context *ctx, uint8_t *base) {
    if (retro_of(ctx->r[3])) { ctx->r[3] = 0; return; }
    orig_fn_8012E400(ctx, base);
}

/* ---- no particles ----
 * Every effect made while a retro player runs (its executeMain, its draw) is dropped at the
 * effect library (mEf::createEffect and the effect_c / levelEffect_c ones), not only the
 * player's own dEf::createPlayerEffect calls (those are above). RT_EFFECT_LOG=1 logs the
 * effects made (name, who). */
static int map_effect_is_retro(ppc_context *ctx, int name_reg, int pos_reg);
static int effect_name_arg(int which) { return which == 0 ? 3 : 4; }
static int fx_drop(ppc_context *ctx, int which) {
    static int none = -1; if (none < 0) none = getenv("RT_NO_EFFECTS") != NULL;   /* testing: no effects at all */
    int drop = fx_block > 0 || none ? 1
             : which == 0 && ctx->lr == 0x801033ECu ? map_effect_is_retro(ctx, 3, 5)   /* (dWmEffectManager_c::playEffect: the world map) */
             : which == 1 ? map_effect_is_retro(ctx, 4, 0) : 0;                     /* (the map also makes some straight through effect_c) */
    static int lg = -1; if (lg < 0) lg = getenv("RT_EFFECT_LOG") != NULL;
    if (lg) {
        uint32_t np = ctx->r[effect_name_arg(which)]; char nm[48] = "?"; int i;
        if (np >= 0x80000000u && np < 0x81800000u) { for (i = 0; i < 47 && R8(np + i); i++) nm[i] = (char)R8(np + i); nm[i] = 0; }
        rt_log("effect", "%s%s (lr %08X, frame %llu)", nm, drop ? " - dropped (retro)" : "", ctx->lr, (unsigned long long)g_frames_presented);
    }
    return drop;
}
#define FX_HOOK(fn, which) void hle_##fn(ppc_context *ctx, uint8_t *base) { \
    if (fx_drop(ctx, which)) { ctx->r[3] = 0; return; } orig_##fn(ctx, base); }
FX_HOOK(createEffect__3mEfFPCcUlPC7mVec3_cPC7mAng3_cPC7mVec3_c, 0)
FX_HOOK(createEffect__Q23mEf8effect_cFPCci, 1)
FX_HOOK(createEffect__Q23mEf8effect_cFPCcUlPC7mVec3_cPC7mAng3_cPC7mVec3_c, 1)
FX_HOOK(createEffect__Q23mEf8effect_cFPCcUlPC6mMtx_c, 1)
FX_HOOK(createEffect__Q23mEf13levelEffect_cFPCci, 1)
FX_HOOK(createEffect__Q23mEf13levelEffect_cFPCcUlPC7mVec3_cPC7mAng3_cPC7mVec3_c, 1)
FX_HOOK(createEffect__Q23mEf13levelEffect_cFPCcUlPC6mMtx_c, 1)
/* EGG::Effect::create (all of the above end here; some code calls it straight) */
void hle_create__Q23EGG6EffectFv(ppc_context *ctx, uint8_t *base) {
    static int lg = -1; if (lg < 0) lg = getenv("RT_EFFECT_LOG") != NULL;
    if (lg) { uint32_t e = ctx->r[3]; char nm[33] = ""; int i;
        for (i = 0; i < 32 && R8(e + 4 + i) >= 0x20 && R8(e + 4 + i) < 0x7F; i++) nm[i] = (char)R8(e + 4 + i); nm[i] = 0;
        rt_log("effect", "EGG create %s%s (lr %08X)", nm, fx_block ? " - dropped (retro)" : "", ctx->lr); }
    if (fx_block > 0 || getenv("RT_NO_EFFECTS")) { ctx->r[3] = 0; return; }
    orig_create__Q23EGG6EffectFv(ctx, base);
}

/* ---- sounds ----
 * A retro player makes SMB1's sounds (rt_sfx.c, ui/retro/mario/sfx), and only for what it does:
 *   jump / jumpsmall, stompswim (a swim stroke, a stomp) - from the SMB1 physics (above);
 *   bump, brick, item, coin - a block hit with its head: NSMBW's sound for what the block did
 *     (break, an item, a coin) within a few frames says which, else a bump;
 *   coin - a coin it touched (NSMBW's coin sound from the player's own update);
 *   powerup, pipepowerdown (also entering a pipe), death - from its form and state (above);
 *   fireball, 1up - NSMBW's throw-fire / 1-up sounds while it is the one acting.
 * NSMBW's own sound for those (and all of the retro player's own player sounds and voice) is not
 * played. Sound IDs (sound_list.h of the NSMBW decomp): */
enum { SE_SYS_ONE_UP = 177, SE_SYS_ONE_UP_RC = 178, SE_SYS_100COIN_ONE_UP = 179, SE_PLY_THROW_FIRE = 279,
       SE_OBJ_GET_COIN = 567, SE_OBJ_GET_COIN_SHOWER = 568, SE_OBJ_ITEM_APPEAR = 599, SE_PLY_HIT_BLOCK = 626,
       SE_PLY_HIT_BLOCK_BOUND = 627, SE_OBJ_BLOCK_BREAK = 628 };
static int retro_sound(uint32_t id, uint32_t obj) {
    /* 1 = NSMBW's sound is replaced (not played) */
    for (int i = 0; i < 8; i++) {
        retro_t *r = &R[i];
        if (!r->actor || retro_of(r->actor) != r) continue;
        int own = obj >= r->actor && obj < r->actor + 0x2A80;              /* the player's own sound object */
        if (r->bump_t && (id == SE_OBJ_BLOCK_BREAK || id == SE_OBJ_ITEM_APPEAR || id == SE_OBJ_GET_COIN)) {
            if (!r->bump_sound) { r->bump_sound = (int)id; sfx_play(retro_dir(), id == SE_OBJ_BLOCK_BREAK ? "brick" : id == SE_OBJ_ITEM_APPEAR ? "item" : "coin"); }
            return 1;
        }
        if (r->bump_t && (id == SE_PLY_HIT_BLOCK || id == SE_PLY_HIT_BLOCK_BOUND)) return 1;
        if (acting == r || own) {
            if (id == SE_OBJ_GET_COIN || id == SE_OBJ_GET_COIN_SHOWER) { sfx_play(retro_dir(), "coin"); r->obj_sound_frame = g_frames_presented; return 1; }
            if (id == SE_OBJ_BLOCK_BREAK || id == SE_OBJ_ITEM_APPEAR) {
                sfx_play(retro_dir(), id == SE_OBJ_BLOCK_BREAK ? "brick" : "item"); r->obj_sound_frame = g_frames_presented; return 1; }
            if (id == SE_SYS_ONE_UP || id == SE_SYS_ONE_UP_RC || id == SE_SYS_100COIN_ONE_UP) { sfx_play(retro_dir(), "1up"); return 1; }
            if (id == SE_PLY_THROW_FIRE) { sfx_play(retro_dir(), "fireball"); return 1; }
            if (own) return 1;                                          /* (NSMBW's player sounds and voice) */
        }
    }
    return 0;
}
static int se_log(ppc_context *ctx, const char *what, uint32_t id) {
    static int lg = -1; if (lg < 0) lg = getenv("RT_SE_LOG") != NULL;
    if (lg) rt_log("se", "%s %u obj %08X lr %08X acting %d frame %llu", what, id, ctx->r[3], ctx->lr, acting ? (int)(acting - R) : -1, (unsigned long long)g_frames_presented);
    return lg;
}
/* nw4r::snd::SoundStartable::detail_StartSound / detail_HoldSound(handle r4, id r5, info r6) */
void hle_detail_StartSound__Q34nw4r3snd14SoundStartableFPQ34nw4r3snd11SoundHandleUlPCQ44nw4r3snd14SoundStartable9StartInfo(ppc_context *ctx, uint8_t *base) {
    se_log(ctx, "start", ctx->r[5]);
    if (retro_sound(ctx->r[5], ctx->r[3])) { ctx->r[3] = 1; return; }    /* (START_ERR_LOW_PRIORITY: not played) */
    orig_detail_StartSound__Q34nw4r3snd14SoundStartableFPQ34nw4r3snd11SoundHandleUlPCQ44nw4r3snd14SoundStartable9StartInfo(ctx, base);
}
void hle_detail_HoldSound__Q34nw4r3snd14SoundStartableFPQ34nw4r3snd11SoundHandleUlPCQ44nw4r3snd14SoundStartable9StartInfo(ppc_context *ctx, uint8_t *base) {
    uint32_t obj = ctx->r[3];
    for (int i = 0; i < 8; i++) if (R[i].actor && obj >= R[i].actor && obj < R[i].actor + 0x2A80 && retro_of(R[i].actor)) { ctx->r[3] = 1; return; }   /* (the player's held sounds) */
    orig_detail_HoldSound__Q34nw4r3snd14SoundStartableFPQ34nw4r3snd11SoundHandleUlPCQ44nw4r3snd14SoundStartable9StartInfo(ctx, base);
}
/* daPyMng_c::startMissBGM(player): the SMB1 death sound instead */
void hle_startMissBGM__9daPyMng_cFi(ppc_context *ctx, uint8_t *base) {
    int n = (int)ctx->r[3];
    if (n >= 0 && n < 8 && R[n].actor && retro_of(R[n].actor)) return;
    orig_startMissBGM__9daPyMng_cFi(ctx, base);
}

/* ================================================================== drawing */
static void pick_frame(ppc_context *ctx, uint8_t *base, retro_t *r, uint32_t pl) {
    if (r->goal.phase) return;                                          /* (goal_step picks it) */
    if (r->vine.on) {
        r->frame = r->vine.anim ? FR_CLIMB2 : FR_CLIMB1;
        r->facing_left = r->vine.side;
        return;
    }
    if (r->controlled) {
        smb1_gfx g; smb1_graphics(&r->p, &g);
        switch (g.gfx) {
        case SMB1_GFX_JUMP:   r->frame = FR_JUMP; break;
        case SMB1_GFX_STAND:  r->frame = FR_IDLE; break;
        case SMB1_GFX_SKID:   r->frame = FR_SKID; break;
        case SMB1_GFX_CROUCH: r->frame = FR_CROUCH; break;
        case SMB1_GFX_FIRE:   r->frame = FR_SHOOT; break;
        case SMB1_GFX_CLIMB:  r->frame = g.anim ? FR_CLIMB2 : FR_CLIMB1; break;
        case SMB1_GFX_SWIM:
            /* the sheets have 4 swim pictures, each with and without the kicking feet (swim1/2,
               3/4, 5/6, 7/8): a new picture each time SMB1's swim animation steps */
            if (g.anim != r->swim_last_anim) { r->swim_last_anim = g.anim; r->swim_pic = (r->swim_pic + 1) & 3; }
            r->frame = FR_SWIM1 + 2 * r->swim_pic + (g.kick ? 1 : 0);
            break;
        default: {
            /* SMB1's walk frames 1-3 (PlayerGraphicsTable) are walk2, walk1, walk3 on the sheet */
            static const int walk[3] = {FR_WALK2, FR_WALK1, FR_WALK3};
            r->frame = walk[g.anim > 2 ? 2 : g.anim];
        }
        }
        r->facing_left = r->p.PlayerFacingDir == SMB1_FACE_LEFT;
        return;
    }
    /* moved by NSMBW: a picture from the state */
    r->facing_left = R8(pl + PL_DIRECTION) != 0;
    const char *st = R8(pl + PL_IS_DEMO) ? state_name(ctx, base, pl + PL_DEMO_MGR) : state_name(ctx, base, pl + PL_STATE_MGR);
    uint32_t bg = R32(pl + PL_BG_CROSS1);
    float vx = RF(pl + PL_SPEED), vy = RF(pl + PL_SPEED + 4);
    if (strstr(st, "Down") || strstr(st, "Dead")) { r->frame = FR_DEATH; return; }
    if (strstr(st, "Vine") || strstr(st, "Pole") || strstr(st, "Ivy")) {     /* (nets, walls; vines: above) */
        r->anim_t++; r->frame = (fabsf(vx) + fabsf(vy) > 0.01f && (r->anim_t / 8) & 1) ? FR_CLIMB2 : FR_CLIMB1; return;
    }
    if (strstr(st, "Hang")) { r->anim_t++; r->frame = fabsf(vx) > 0.01f && (r->anim_t / 8) & 1 ? FR_HANG2 : FR_HANG1; return; }
    if (strstr(st, "Goal")) { r->frame = (bg & BGC_FOOT) ? FR_IDLE : FR_FLAGPOLE1; return; }
    if (!(bg & BGC_FOOT) && fabsf(vy) > 0.01f) { r->frame = strstr(st, "Swim") ? FR_SWIM1 : FR_JUMP; return; }
    if (fabsf(vx) > 0.05f) {
        static const int walk[3] = {FR_WALK2, FR_WALK1, FR_WALK3};
        r->anim_t++; r->frame = walk[(r->anim_t / 7) % 3]; return;
    }
    r->frame = FR_IDLE;
}

/* one sprite quad: q = top-left, top-right, bottom-right, bottom-left in the space of the
   position matrix at mtx (guest address) */
static void sprite_draw(ppc_context *ctx, uint8_t *base, sheet_t *sh, int f, int facing_left, uint32_t mtx, const float q[4][3], int on_top) {
    /* GX state (the scene's own state is invalidated around it) */
    gcall(ctx, base, A_G3D_INVALIDATE, 1, 0x7FF);
    gcall(ctx, base, A_DISABLE_IND_TEX, 0);
    gcall(ctx, base, A_GXClearVtxDesc, 0);
    gcall(ctx, base, A_GXSetVtxDesc, 2, 9, 1);                 /* POS direct */
    gcall(ctx, base, A_GXSetVtxDesc, 2, 13, 1);                /* TEX0 direct */
    gcall(ctx, base, A_GXSetVtxAttrFmt, 5, 0, 9, 1, 4, 0);        /* fmt 0: POS XYZ f32 */
    gcall(ctx, base, A_GXSetVtxAttrFmt, 5, 0, 13, 1, 4, 0);       /* TEX0 ST f32 */
    gcall(ctx, base, A_GXSetNumChans, 1, 0);
    gcall(ctx, base, A_GXSetNumTexGens, 1, 1);
    gcall(ctx, base, A_GXSetTexCoordGen2, 6, 0, 1, 4, 60, 0, 125);   /* TEXCOORD0 = TEX0, identity */
    gcall(ctx, base, A_GXSetNumIndStages, 1, 0);
    gcall(ctx, base, A_GXSetTevDirect, 1, 0);
    gcall(ctx, base, A_GXSetNumTevStages, 1, 1);
    gcall(ctx, base, A_GXSetTevOrder, 4, 0, 0, 0, 0xFF);      /* stage 0: texcoord 0, map 0, no color */
    gcall(ctx, base, A_GXSetTevOp, 2, 0, 3);                    /* GX_REPLACE */
    gcall(ctx, base, A_GXSetAlphaCompare, 5, 4, 0, 0, 7, 0);    /* alpha > 0 */
    gcall(ctx, base, A_GXSetBlendMode, 4, 0, 1, 0, 0);          /* no blending */
    if (on_top) gcall(ctx, base, A_GXSetZMode, 3, 0, 7, 0);       /* over everything (no depth test) */
    else gcall(ctx, base, A_GXSetZMode, 3, 1, 3, 1);            /* LEQUAL, write */
    gcall(ctx, base, A_GXSetZCompLoc, 1, 0);                    /* depth after the alpha test */
    gcall(ctx, base, A_GXSetCullMode, 1, 0);
    gcall(ctx, base, A_GXSetColorUpdate, 1, 1);
    uint32_t to = RM_TEXOBJ + (uint32_t)(sh - sheets) * 0x20;
    gcall(ctx, base, A_GXInitTexObj, 8, to, sh->tex, sh->tw, sh->th, 6, 0, 0, 0);   /* RGBA8, clamp */
    ctx->f[1].d = 0.0; ctx->f[2].d = 0.0; ctx->f[3].d = 0.0;
    gcall(ctx, base, A_GXInitTexObjLOD, 8, to, 0, 0, 0, 0, 0, 0, 0);               /* nearest */
    gcall(ctx, base, A_GXLoadTexObj, 2, to, 0);
    gcall(ctx, base, A_GXLoadPosMtxImm, 2, mtx, 0);
    gcall(ctx, base, A_GXSetCurrentMtx, 1, 0);
    float u0 = (float)sh->fr[f][0] / sh->tw, u1 = (float)(sh->fr[f][0] + sh->bw) / sh->tw;
    float v0 = (float)sh->fr[f][1] / sh->th, v1 = (float)(sh->fr[f][1] + sh->bh) / sh->th;
    if (facing_left) { float t = u0; u0 = u1; u1 = t; }
    const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    gcall(ctx, base, A_GXBegin, 3, 0x80, 0, 4);                 /* GX_QUADS, fmt 0, 4 vertices */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) gx_fifo_write(fbits(q[i][j]), 4);
        for (int j = 0; j < 2; j++) gx_fifo_write(fbits(uv[i][j]), 4);
    }
    gcall(ctx, base, A_G3D_INVALIDATE, 1, 0x7FF);
}
/* the frame to show, or the sheet's nearest replacement (-1: none) */
static int sheet_frame(sheet_t *sh, int f) { while (f >= 0 && sh->fr[f][0] < 0) f = frame_fallback[f]; return f; }
/* the current camera's view matrix into RM_MTX (0: no camera) */
static int camera_mtx(ppc_context *ctx, uint8_t *base) {
    uint32_t cam = gcall(ctx, base, A_GET_CAMERA_BY_ID, 1, gcall(ctx, base, A_GET_CUR_CAMERA, 0));
    if (!cam) return 0;
    W32(RM_CAMERA, cam);
    gcall(ctx, base, A_GET_CAMERA_MTX, 2, RM_CAMERA, RM_MTX);
    return 1;
}

static void proc_draw(ppc_context *ctx, uint8_t *base, uint32_t proc, int draw_xlu) {
    retro_t *r = NULL;
    for (int i = 0; i < 8; i++) if (R[i].proc == proc) r = &R[i];
    if (!r || !r->draw_pending || !r->actor || r->draw_on_top != draw_xlu) return;
    r->draw_pending = 0;
    uint32_t pl = r->actor;
    if (r->goal.phase && r->goal.hidden) return;
    sheet_t *sh = sheet_for((int)R32(pl + PL_MODE));
    if (!sh) return;
    int f = sheet_frame(sh, r->frame);
    if (f < 0 || !camera_mtx(ctx, base)) return;
    /* the quad: box centred on the player, feet at its position */
    float px = RF(pl + PL_POS), py = RF(pl + PL_POS + 4), pz = RF(pl + PL_POS + 8);
    if (r->goal.phase) { px = r->goal.x; py = r->goal.y; }
    float x0 = px - sh->bw / 2.0f, x1 = px + sh->bw / 2.0f, y0 = py, y1 = py + sh->bh;
    const float q[4][3] = {{x0, y1, pz}, {x1, y1, pz}, {x1, y0, pz}, {x0, y0, pz}};
    sprite_draw(ctx, base, sh, f, r->facing_left, RM_MTX, q, draw_xlu);
}
static void native_proc_dtor(ppc_context *ctx, uint8_t *base) { (void)base; (void)ctx; }
static void native_proc_draw_opa(ppc_context *ctx, uint8_t *base) { proc_draw(ctx, base, ctx->r[3], 0); }
static void native_proc_draw_xlu(ppc_context *ctx, uint8_t *base) { proc_draw(ctx, base, ctx->r[3], 1); }

static void procs_init(void) {
    static int done; if (done) return;
    done = 1;
    rt_register_native(N_PROC_DTOR, native_proc_dtor);
    rt_register_native(N_PROC_DRAW_OPA, native_proc_draw_opa);
    rt_register_native(N_PROC_DRAW_XLU, native_proc_draw_xlu);
    /* vtable: RTTI, 0, dtor, getType, remove, entry, drawOpa, drawXlu */
    const uint32_t vt[8] = {0, 0, N_PROC_DTOR, A_LEAF_GETTYPE, A_LEAF_FREE, A_LEAF_ENTRY, N_PROC_DRAW_OPA, N_PROC_DRAW_XLU};
    for (int i = 0; i < 8; i++) W32(RM_VTABLE + i * 4, vt[i]);
}

/* dAcPy_c::draw: a retro player is drawn by its proc instead of the 3D model */
void hle_fn_80145A60(ppc_context *ctx, uint8_t *base) {
    uint32_t pl = ctx->r[3];
    retro_t *r = retro_of(pl);
    if (!r) { orig_fn_80145A60(ctx, base); return; }
    if (gcall(ctx, base, A_TEST_FLAG, 2, pl, FLAG_NO_DRAW)) {     /* (as the original: skip a frame) */
        gcall(ctx, base, A_CLEAR_FLAG, 2, pl, FLAG_NO_DRAW);
        ctx->r[3] = 1; return;
    }
    procs_init();
    int n = (int)(r - R);
    if (!r->proc) {
        uint32_t proc = RM_PROC + (uint32_t)n * 0x10;
        W32(proc, RM_VTABLE); W32(proc + 4, 0);
        if (!gcall(ctx, base, A_PROC_SETUP, 3, proc, 0, 0)) { rt_log("retro", "draw proc setup failed"); ctx->r[3] = 1; return; }
        r->proc = proc;
    }
    pick_frame(ctx, base, r, pl);
    r->draw_pending = 1;
    (void)draw_ctx;
    gcall(ctx, base, A_LEAF_ENTRY, 1, r->proc);
    ctx->r[3] = 1;
}

/* ================================================================== player models outside levels
 * Everywhere else a player model is drawn (world map, character select figure, ...) it goes
 * through dPyMdlMng_c::draw. A retro character's model is replaced by its sprite there too: a
 * quad facing the camera, at the model's position and size, facing left or right only
 * (world map: the way it last walked left or right; character select: the way the player last
 * pressed left or right; elsewhere: the model's facing). */
#define MDL_ID          0x150       /* u8: player number (levels, scene 0) or player type (elsewhere) */
#define MDL_SCALE_ID    0x152       /* u8: index of its scale (A_MDL_SCALES) */
#define MDL_MTX         0x0D8       /* MTX34: where it is drawn */
#define MDL_SCENE       0x180       /* dPyMdlMng_c::SceneType_e: 0 level, 1 world map, 2 select figure */
#define A_MDL_SCALES    0x80370DA8u /* float[]: each model's scale in levels */
#define MOD_PY_TYPE     0x8185E93Cu /* daPyMng_c::mPlayerType[8] (by player) */
#define MOD_PY_MODE     0x8185E95Cu /* daPyMng_c::mPlayerMode[8] (by type) */
#define RM_MPROC        (RETRO_MEM + 0x0400)   /* 16 x 0x10: one draw proc per model */
#define RM_MVTABLE      (RETRO_MEM + 0x0140)
#define RM_IDMTX        (RETRO_MEM + 0x02A0)   /* identity MTX34 */
#define N_MPROC_DRAW_OPA 0x80003F0Cu
#define MS_COUNT        16
typedef struct {
    uint32_t mng, proc;
    int pending, scene, type, frame, facing_left, anim_t, have_last;
    float pos[3], k, last[3];        /* model position, size against a level's, last position */
} msprite_t;
static msprite_t MS[MS_COUNT];

/* the player slot a model belongs to when that slot plays SMB1 Mario (-1: not a retro model) */
#define MDL_CHARA_ID    0x151       /* u8: dPyMdlMng_c::ModelType_e (5: Yoshi) */
static int smb1_slot_of_model(uint32_t mdl) {
    int id = R8(mdl + MDL_ID), scene = (int)R32(mdl + MDL_SCENE);
    if (R8(mdl + MDL_CHARA_ID) == 5) return -1;                           /* a Yoshi (its +0x150 is its rider's number) */
    if (R32(G_GAME_MODE) == 2) return -1;                                /* (title screen) */
    if (scene == 0) return id < 8 && chars_is_smb1(id) ? id : -1;
    for (int n = 0; n < 8; n++) if ((int)R32(MOD_PY_TYPE + 4 * n) == id && chars_is_smb1(n)) return n;
    return -1;
}
static msprite_t *ms_of(uint32_t mng, int add) {
    for (int i = 0; i < MS_COUNT; i++) if (MS[i].mng == mng) return &MS[i];
    if (!add) return NULL;
    for (int i = 0; i < MS_COUNT; i++) if (!MS[i].mng) { memset(&MS[i], 0, sizeof MS[i]); MS[i].mng = mng; return &MS[i]; }
    return NULL;
}
static void mproc_draw(ppc_context *ctx, uint8_t *base, uint32_t proc) {
    msprite_t *m = NULL;
    for (int i = 0; i < MS_COUNT; i++) if (MS[i].mng && MS[i].proc == proc) m = &MS[i];
    if (!m || !m->pending) return;
    m->pending = 0;
    sheet_t *sh = sheet_for((int)R32(MOD_PY_MODE + 4 * m->type));
    if (!sh) return;
    int f = sheet_frame(sh, m->frame);
    if (f < 0 || !camera_mtx(ctx, base)) return;
    /* the position in view space; the quad is upright in it (faces the camera) */
    float c[12]; for (int i = 0; i < 12; i++) c[i] = RF(RM_MTX + 4 * i);
    float vx = c[0] * m->pos[0] + c[1] * m->pos[1] + c[2] * m->pos[2] + c[3];
    float vy = c[4] * m->pos[0] + c[5] * m->pos[1] + c[6] * m->pos[2] + c[7];
    float vz = c[8] * m->pos[0] + c[9] * m->pos[1] + c[10] * m->pos[2] + c[11] + 2.0f * m->k;   /* (a little in front) */
    float hw = sh->bw / 2.0f * m->k, h = sh->bh * m->k;
    const float q[4][3] = {{vx - hw, vy + h, vz}, {vx + hw, vy + h, vz}, {vx + hw, vy, vz}, {vx - hw, vy, vz}};
    static const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (int i = 0; i < 12; i++) WF(RM_IDMTX + 4 * i, id[i]);
    sprite_draw(ctx, base, sh, f, m->facing_left, RM_IDMTX, q, 0);
}
static void native_mproc_draw_opa(ppc_context *ctx, uint8_t *base) { mproc_draw(ctx, base, ctx->r[3]); }

/* dPyMdlMng_c::draw */
void hle_draw__11dPyMdlMng_cFv(ppc_context *ctx, uint8_t *base) {
    uint32_t mng = ctx->r[3], mdl = R32(mng + 4);
    int slot = mdl && R32(mng + 8) ? smb1_slot_of_model(mdl) : -1;
    msprite_t *m = slot >= 0 ? ms_of(mng, 1) : NULL;
    if (!m) { orig_draw__11dPyMdlMng_cFv(ctx, base); return; }
    procs_init();
    if (!m->proc) {
        static int vt_done;
        if (!vt_done) { vt_done = 1; rt_register_native(N_MPROC_DRAW_OPA, native_mproc_draw_opa);
            const uint32_t vt[8] = {0, 0, N_PROC_DTOR, A_LEAF_GETTYPE, A_LEAF_FREE, A_LEAF_ENTRY, N_MPROC_DRAW_OPA, N_PROC_DRAW_XLU};
            for (int i = 0; i < 8; i++) W32(RM_MVTABLE + i * 4, vt[i]); }
        uint32_t proc = RM_MPROC + (uint32_t)(m - MS) * 0x10;
        W32(proc, RM_MVTABLE); W32(proc + 4, 0);
        if (!gcall(ctx, base, A_PROC_SETUP, 3, proc, 0, 0)) { rt_log("retro", "model draw proc setup failed"); return; }
        m->proc = proc;
        rt_log("retro", "player %d's model (scene %u) drawn as its sprite", slot, R32(mdl + MDL_SCENE));
    }
    m->scene = (int)R32(mdl + MDL_SCENE);
    m->type = m->scene == 0 ? (int)R32(MOD_PY_TYPE + 4 * slot) : R8(mdl + MDL_ID);
    if (m->type < 0 || m->type >= 8) m->type = 0;
    /* where: the model matrix's position; its size against the model's size in levels */
    float t[12]; for (int i = 0; i < 12; i++) t[i] = RF(mdl + MDL_MTX + 4 * i);
    float s = sqrtf(t[0] * t[0] + t[4] * t[4] + t[8] * t[8]), ls = RF(A_MDL_SCALES + 4 * R8(mdl + MDL_SCALE_ID));
    m->pos[0] = t[3]; m->pos[1] = t[7]; m->pos[2] = t[11];
    m->k = ls > 0.0001f && s > 0.0001f ? s / ls : 1.0f;
    /* which way: left or right only */
    if (m->scene == 2) {
        m->facing_left = chars_select_facing(slot) < 0;
    } else if (camera_mtx(ctx, base)) {
        float fx = t[2] / s, fy = t[6] / s, fz = t[10] / s;               /* the model's forward (+z) */
        float vfx = RF(RM_MTX) * fx + RF(RM_MTX + 4) * fy + RF(RM_MTX + 8) * fz;
        if (vfx > 0.35f) m->facing_left = 0; else if (vfx < -0.35f) m->facing_left = 1;
    }
    /* picture: walking while it moves, jumping while it rises or falls */
    float dx = 0, dy = 0, dz = 0;
    if (m->have_last) { dx = (m->pos[0] - m->last[0]) / m->k; dy = (m->pos[1] - m->last[1]) / m->k; dz = (m->pos[2] - m->last[2]) / m->k; }
    memcpy(m->last, m->pos, sizeof m->last); m->have_last = 1;
    float hv = sqrtf(dx * dx + dz * dz);
    if (fabsf(dy) > 0.6f && fabsf(dy) > hv) m->frame = FR_JUMP;
    else if (hv > 0.1f) { static const int walk[3] = {FR_WALK2, FR_WALK1, FR_WALK3}; m->anim_t++; m->frame = walk[(m->anim_t / 6) % 3]; }
    else { m->frame = FR_IDLE; m->anim_t = 0; }
    m->pending = 1;
    gcall(ctx, base, A_LEAF_ENTRY, 1, m->proc);
}
/* dPyMdlMng_c::~dPyMdlMng_c: its sprite's draw proc goes with the scene */
void hle___dt__11dPyMdlMng_cFv(ppc_context *ctx, uint8_t *base) {
    msprite_t *m = ms_of(ctx->r[3], 0);
    if (m) memset(m, 0, sizeof *m);
    orig___dt__11dPyMdlMng_cFv(ctx, base);
}

/* the world map's effects for a player (landing / footstep smoke, splashes) where a retro
   character's sprite stands: none (its other effects - enemies, events - stay) */
static int map_effect_is_retro(ppc_context *ctx, int name_reg, int pos_reg) {
    static const char *mine[] = {"Wm_cs_mrlandsmoke", "Wm_cs_mrsandlandsmoke", "Wm_cs_comnlandsmoke", "Wm_cs_footsmoke",
                                 "Wm_cs_footsand01", "Wm_cs_footsand02", "Wm_cs_footsoil", "Wm_cs_footice",
                                 "Wm_cs_jumpwater", "Wm_cs_landwater", NULL};
    uint32_t np = ctx->r[name_reg], pp = pos_reg ? ctx->r[pos_reg] : 0;
    char nm[32] = ""; int i;
    if (np < 0x80000000u || np >= 0x81800000u) return 0;
    for (i = 0; i < 31 && R8(np + i); i++) nm[i] = (char)R8(np + i); nm[i] = 0;
    int ok = 0; for (i = 0; mine[i]; i++) if (!strcmp(nm, mine[i])) ok = 1;
    if (!ok) return 0;
    for (i = 0; i < MS_COUNT; i++) {
        msprite_t *m = &MS[i];
        if (!m->mng || m->scene != 1) continue;
        if (pp < 0x80000000u) return 1;                                 /* (no position: a retro character is on the map) */
        float dx = RF(pp) - m->pos[0], dy = RF(pp + 4) - m->pos[1], dz = RF(pp + 8) - m->pos[2], r = 40.0f * (m->k > 0 ? m->k : 1);
        if (dx * dx + dy * dy + dz * dz < r * r) return 1;
    }
    return 0;
}
