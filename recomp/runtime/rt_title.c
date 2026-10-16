/* rt_title.c - the title screen's layout (dEventOpeningTitle_c), native.
 *
 * Part of moving the 8-player module to native C (NATIVE_PORT.md; MPL-2.0, see
 * THIRD_PARTY_NOTICES.md). The module's create() wrote "<project> <version>" from its
 * Version.json into the layout's E3 version text; the 8-player code is part of this port now,
 * so that text stays hidden, as in the original game. */
#include "rt.h"
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

#define A_CREATE_LAYOUT     0x80781D40u   /* dEventOpeningTitle_c::createLayout() */
#define A_DISP_SIZE_SCALE   0x800B50F0u   /* dGameCom::DispSizeScale(VEC2 &) */
#define A_IS_ANIM_ON        0x800C9610u   /* m2d::EmbedLayout_c::isAnimOn(group; -1 any) */
#define A_SET_NEXT_SCENE    0x80102260u   /* dScStage_c::setNextScene(profile, param, exit, fader) */
#define A_START_TITLE       0x801017D0u   /* dScRestartCrsin_c::startTitle(u8, bool) */
#define G_GAME_MODE         0x8042A204u   /* dScStage_c::m_gameMode (2: title) */
#define G_GAME_FLAG         0x80429F80u   /* dInfo_c::mGameFlag */
#define GF_MULTI_MODE       0x10
#define SCENE_GAME_SETUP    10
#define FADER_TITLE_EXIT    5

/* dEventOpeningTitle_c */
#define OT_LAYOUT           0x70
#define OT_ROOT             0x244
#define OT_E3_VER_CHECK     0x248         /* T_E3VerCheck */
#define OT_PROPORTION_C     0x24C         /* N_proportionC */
#define OT_CREATED          0x274         /* m_created, then m_visible */

static void pane_hide(uint32_t p) { if (p) W8(p + 0xBB, R8(p + 0xBB) & ~1); }

/* create */
void hle_mod_fn_2_144E0(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3];
    if (R8(self + OT_CREATED)) { ctx->r[3] = 1; return; }                 /* SUCCEEDED */
    if (!rt_gcall(ctx, base, A_CREATE_LAYOUT, 1, self)) { ctx->r[3] = 0; return; }   /* NOT_READY */
    W16(self + OT_CREATED, 0x0101);                                       /* m_created = m_visible = true */
    pane_hide(R32(self + OT_E3_VER_CHECK));                               /* no version text */
    pane_hide(R32(self + OT_ROOT));
    /* the layout scaled to the display: a VEC2 on the guest stack */
    uint32_t sp = ctx->r[1], vec = sp - 32;
    ctx->r[1] = sp - 64;
    rt_gcall(ctx, base, A_DISP_SIZE_SCALE, 1, vec);
    ctx->r[1] = sp;
    uint32_t prop = R32(self + OT_PROPORTION_C);
    W32(prop + 68, R32(vec)); W32(prop + 72, R32(vec + 4));               /* mScale */
    ctx->r[3] = 1;
}

/* executeState_PressButtonExitAnimeEndWait: 2 was pressed on the title */
void hle_mod_fn_2_14BD0(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3];
    if (R32(G_GAME_MODE) == 2) {
        if (rt_gcall(ctx, base, A_IS_ANIM_ON, 2, self + OT_LAYOUT, (uint32_t)-1)) return;
        rt_gcall(ctx, base, A_SET_NEXT_SCENE, 4, SCENE_GAME_SETUP, 0, 0, FADER_TITLE_EXIT);
        return;
    }
    W32(G_GAME_FLAG, R32(G_GAME_FLAG) & ~(uint32_t)GF_MULTI_MODE);
    rt_gcall(ctx, base, A_START_TITLE, 2, 0, 0);
}

/* dScCrsin_c::executeState_resWaitProc2: the module sets up the title screen's eight players
   here with a random power-up (all small, all mini, or a random one each). The original game's
   title has everyone in Super Mushroom form, so after the module's setup they are put back to
   that. (The rest of the function - credits, pipe randomizer - is the module's, unchanged.) */
#define G_START_DEMO_TYPE   0x80315B98u   /* dInfo_c::m_startGameInfo.demoType (2: title) */
#define MOD_PY_MODE         0x8185E95Cu   /* daPyMng_c::mPlayerMode[8] (by player type) */
#define PLAYER_MODE_MUSHROOM 1
void hle_mod_fn_2_1B2680(ppc_context *ctx, uint8_t *base) {
    int title = R32(G_START_DEMO_TYPE) == 2;
    orig_mod_fn_2_1B2680(ctx, base);
    if (title) {
        for (int t = 0; t < 8; t++) W32(MOD_PY_MODE + 4 * t, PLAYER_MODE_MUSHROOM);
        chars_title_players();                          /* the first five characters (rt_chars.c) */
    }
}
