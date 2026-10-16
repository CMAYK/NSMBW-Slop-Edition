/* rt_gamesetup.c - the game setup scene (dScGameSetup_c), native.
 *
 * Flow from the title menu (Singleplayer / Multiplayer):
 *     Game Select -> File Select -> character select -> game
 * Back (1) goes the other way: character select -> File Select -> Game Select -> title.
 * Singleplayer character select: Start Game (alone, centred) shows only once the character is
 * chosen; 1 then takes the character back (the card returns on it), and 1 again goes back.
 *
 * Game Select and File Select are the game's file select screen (dFileSelect_c) shown two
 * ways: as Game Select it has a single card, "NSMBW", in the middle (more games - level and map
 * packs - come later). The "Select Players" screen (dSelectPlayer_c) is gone: the character
 * select (dNumberOfPeopleChange_c) opens with 1 player (Singleplayer) or 8 (Multiplayer).
 * Every change of screen goes through black: the screen fades out with everything in place,
 * the next screen is set up while it is black, and it fades in already in place.
 *
 * This is also the first part of the 8-player module (MPL-2.0, THIRD_PARTY_NOTICES.md) moved to native
 * C: all of dScGameSetup_c, and the parts of the character select the menus need. The
 * recompiled originals stay available as orig_<name>() (see NATIVE_PORT.md).
 *
 * Guest functions are called with gcall(); state machines are driven through the state
 * manager's vtable (sStateMgr: +0x18 changeState, +0x28 getStateID). */
#include "rt.h"
#include <math.h>
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */
extern uint64_t g_frames_presented;
int menu_singleplayer(void);                    /* rt_menu.c: 1 Singleplayer, 0 Multiplayer */
void menu_controls_open(void);                  /* rt_menu.c: the Controls screen (overlay) */
int menu_controls_is_open(void);

/* ---- guest addresses (USA rev 2) ---- */
#define G_NOW_SCENE       0x80428732u   /* dScene_c::m_nowScene */
#define SCENE_WORLD_MAP   3
#define SCENE_GAME_SETUP  10
#define G_FADER           0x8042A440u   /* dFader_c::mFader (vtable +0x10 getStatus) */
enum { FADER_OPAQUE = 0, FADER_HIDDEN = 1 };
#define A_SET_FADER       0x800B0DB0u   /* dFader_c::setFader(type) */
#define A_FADE_IN         0x800B0A20u   /* dFader_c::startFadeIn(frames) */
#define A_FADE_OUT        0x800B0A90u   /* dFader_c::startFadeOut(frames) */
#define A_START_TITLE     0x801017D0u   /* dScRestartCrsin_c::startTitle(u8, bool) */
#define A_INIT_LOAD_GAME  0x800E1840u   /* dSaveMng_c::initLoadGame(s8) */
#define A_LOAD_QUICK_SAVE 0x800E1940u   /* dSaveMng_c::loadQuickSaveData(u8) */
#define A_GET_SAVE_GAME   0x800E0380u   /* dSaveMng_c::getSaveGame(s8) */
#define G_SAVE_MNG        0x8042A040u   /* dSaveMng_c::m_instance */
#define SM_LAST_FILE      38            /* (s8) last save file played */
#define A_CHECK_BONUS_NOCAP 0x80060EF0u /* daPyMng_c::checkBonusNoCap() */
#define G_INFO            0x80429F7Cu   /* dInfo_c::m_instance */
#define G_GAME_FLAG       0x80429F80u   /* dInfo_c::mGameFlag */
enum { GF_MULTI_MODE = 0x10, GF_FREE_FOR_ALL = 0x20, GF_COIN_BATTLE = 0x40 };
#define G_SND_AUDIO_MGR   0x8042A488u   /* SndAudioMgr::sInstance */
#define A_START_SYSTEM_SE 0x80195380u   /* SndAudioMgr::startSystemSe(id, count) */
enum { SE_SYS_DECIDE = 121, SE_SYS_BACK = 122 };
#define G_SND_SCENE_MGR   0x8042A4A8u   /* SndSceneMgr::sInstance */
#define A_SET_SETUP_TRACK 0x8019BA20u   /* SndSceneMgr::setGameSetupTrack(track) */
#define A_CHK_CANCEL      0x800B53A0u   /* dGameCom::chkCancelButton(player) */
#define A_IS_ANIM_ON      0x800C9610u   /* m2d::EmbedLayout_c::isAnimOn(group; -1 any) */
#define A_CREATE_CHILD    0x80162B00u   /* fBase_c::createChild(profile, parent, param, group) */
#define A_CONSTRUCT_ACTOR 0x8006CBA0u   /* dBaseActor_c::construct(profile, parent, param, pos, angle) */
#define A_DO_SCENE_CHANGE 0x800E1E60u   /* dScene_c::setNextScene(profile, param, force) */
#define A_WMAP_BOOT_PARAM 0x80102A40u   /* dScWMap_c::CreateBootParam() */
#define A_DATEFILE_ANIME_OUT 0x80918460u /* dScGameSetup_c::executeDateFileAnimeOut() */
#define G_REMOCON_MNG     0x8042A028u   /* dRemoconMng_c::m_instance */
#define G_GAME_KEY        0x80429F50u   /* dGameKey_c::m_instance */
#define G_CUR_CORE        0x8042A464u   /* the player whose remote drives the menus */
#define G_SETUP_STATE_ID  0x80371870u   /* dRemoconMng_c::dConnect_c::StateID_Setup */
#define G_DISABLE_OPENING_MOVIE 0x8000678Au   /* fFeat::disable_opening_movie (settings.ini skip_opening_movie) */
/* data the 8-player module moved to its own area (8 players instead of 4) */
#define MOD_PY_TYPE       0x8185E93Cu   /* daPyMng_c::mPlayerType[8] */
#define MOD_PY_MODE       0x8185E95Cu   /* daPyMng_c::mPlayerMode[8] */
#define MOD_PY_CREATE_ITEM 0x8185E97Cu  /* daPyMng_c::mCreateItem[8] */
#define MOD_PY_REST       0x81851D8Cu   /* daPyMng_c::mRest[8] */
#define MOD_DEFAULT_TYPES 0x81849BE4u   /* dMj2dGame_c::scDefaultPlayerTypes[8] */
#define MOD_OVERLAY_RES   0x8185FA14u   /* mDvd overlay resource flag (setOverlayRes) */
#define MOD_GAME_VERSION  0x8185ED00u   /* region/revision: the profile table differs */
#define MFN_SET_EASY_PAIRING_WAIT 0x8181CCACu /* dNumberOfPeopleChange_c::setEasyPairingWait(bool) */
#define MFN_SPLIT_EXTENSION 0x81812EC4u /* dRemoconMng_c::dConnect_c::splitExtension() */

/* dScGameSetup_c */
#define GS_STATE        0x74
#define GS_2DPLAYER     0xC0            /* [4] */
#define GS_NUMPY        0xB8
#define GS_SELPLY       0xD0
#define GS_EASY_PAIRING 0xD4
#define GS_SEQUENCE_BG  0xD8
#define GS_DATEFILE     0xDC            /* [3] */
#define GS_FSEL         0xE8
#define GS_INFO_WINDOW  0xEC
#define GS_CREATE_IDX   0xF8
#define GS_IN_PLAYER_SELECT 0x108
#define ST_GS_FILE_SELECT    0x809A1548u
#define ST_GS_FADE_OUT       0x809A1588u
#define ST_GS_FADE_IN        0x809A15C8u
#define ST_GS_CONNECTION_CHECK 0x809A1648u
#define ST_GS_EASY_PAIRING_WAIT 0x809A1688u
#define ST_GS_LOW_BATTERY_CHECK 0x809A16C8u
#define ST_GS_NEXT_SCENE_WAIT 0x809A1788u
/* dFileSelect_c */
#define FS_STATE        0x20C
#define ST_FS_START_WAIT 0x80991F58u
#define FS_DATEFILE     0x24C           /* [3] */
#define FS_INFO_WINDOW  0x258
#define FS_CHOICE       0x2A4           /* 0-2 file, 3 Free-for-All, 4 Coin Battle */
#define FS_LAST_CHOICE  0x2CC
#define FS_ACTIVE       0x2D9
#define FS_CHOICE_DONE  0x2DB
#define FS_CHOICE_BACK  0x2DC
#define FS_CHOICE_TEMP  0x2DD
/* dDateFile_c (the file cards) */
#define DF_STATE        0xD9C
#define DF_REQUEST      0xDD8           /* 1 on, 2 picked, 5 reset, ... */
#define DF_ACTIVE       0xE49
#define ST_DF_INITIAL_SETUP 0x80991880u
/* dSelectPlayer_c */
#define SP_DECIDED      0x26B
#define SP_BUTTON       0x26C
#define SP_DIRECTION    0x270
#define SP_EASY_PAIRING_FLAG 0x279      /* (on dEasyPairing_c) */
/* dNumberOfPeopleChange_c (the character select) */
#define NP_LAYOUT       0x404
#define NP_STATE        0x59C
#define NP_ROOT         0x5D8
#define NP_ACTIVE       0x67E
#define NP_CANCEL_OK    0x680
#define NP_PLAYER_COUNT 0x6C4
#define NP_ON_BUTTON    0x6C8           /* 0 Start Game, 1 Back */
#define NP_CC_COUNT     0x710
#define NP_CC_BASE      0x714           /* [8] dCharacterChangeSelectBase_c */
#define NP_CC_CONTENTS  0x734
#define NP_CC_ARROW     0x754
#define NP_CC_INDICATOR 0x774
#define NP_2DPLAYER     0x794           /* [8] by player type */
#define ST_NP_NOW_ENTRANT_RECRUIT   0x809945B0u
#define ST_NP_INFO_WINDOW_EXIT_WAIT 0x80994630u
#define ST_NP_EXIT_ANIME_END_CHECK  0x80994770u
/* dCharacterChangeSelectBase_c */
#define CB_STATE        0x228
#define CB_BUSY         0x296
#define CB_DECIDED      0x29A
#define CB_EXIT         0x29B
#define CB_ENTER_COUNT  0x288           /* int*: how many players have chosen */
#define ST_CB_PLAYER_EXIT_WAIT 0x80990D80u
#define A_ANIM_RESET    0x800C93D0u     /* m2d::EmbedLayout_c::resetAnim(group, at the end) */
/* the character select layout's animation groups (createLayout): 2-5 inYesNoButtons (yes, no,
   guide, guide), 6-7 onYesNoButtons (yes, no) */
enum { NPA_IN_FIRST = 2, NPA_ON_LAST = 7 };
#define ST_CB_SELECT_START_WAIT    0x80990A80u
#define ST_CB_BUTTON_EXIT_ANIME    0x80990AC0u   /* (what 2 starts in SelectStartWait: the card comes back) */
#define NP_IND_PANES    0x21C           /* (on each indicator) two panes the buttons hide: +0x21C, +0x220 */
#define TDP_FLAG        0x266           /* (on each 2D player) set while the buttons show */
#define NP_BUTTONS_FLAG 0x688

/* ---- guest calls (rt_gcall in rt.h) ---- */
uint32_t rt_gcall_(const char *file, int line, ppc_context *ctx, uint8_t *base, uint32_t fn, int n, ...) {
    uint32_t saved[13], lr = ctx->lr, ctr = ctx->ctr;
    if (fn < 0x80000000u) { static int bad; if (bad++ < 8) rt_log("gcall", "%s:%d: no function to call (%08X)", file, line, fn); return 0; }
    memcpy(saved, ctx->r, sizeof saved);
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n && i < 8; i++) ctx->r[3 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    ctx->lr = 0;
    ppc_call_indirect(ctx, base, fn);
    uint32_t ret = ctx->r[3];
    memcpy(ctx->r, saved, sizeof saved); ctx->lr = lr; ctx->ctr = ctr;
    return ret;
}
#define gcall rt_gcall
#define VFN(obj, off) R32(R32(obj) + (off))
static void change_state(ppc_context *ctx, uint8_t *base, uint32_t mgr, uint32_t state) { gcall(ctx, base, VFN(mgr, 0x18), 2, mgr, state); }
static uint32_t state_id(ppc_context *ctx, uint8_t *base, uint32_t mgr) { return gcall(ctx, base, VFN(mgr, 0x28), 1, mgr); }
/* a state object is [vtable][name][number]...; the name is "Class::StateID_Name" */
static int state_is(ppc_context *ctx, uint8_t *base, uint32_t mgr, const char *name) {
    uint32_t st = state_id(ctx, base, mgr), np = st ? R32(st + 4) : 0;
    if (np < 0x80000000u || np >= 0x81800000u) return 0;
    char s[96]; int i;
    for (i = 0; i < 95 && R8(np + i); i++) s[i] = (char)R8(np + i);
    s[i] = 0;
    const char *u = strstr(s, "StateID_");
    return u && !strcmp(u + 8, name);
}
static int fader_seen = FADER_HIDDEN;     /* last status read (input is held while the screen isn't fully shown) */
static int fader_status(ppc_context *ctx, uint8_t *base) { uint32_t f = R32(G_FADER); return fader_seen = f ? (int)gcall(ctx, base, VFN(f, 0x10), 1, f) : FADER_HIDDEN; }
static void fade_out(ppc_context *ctx, uint8_t *base) { gcall(ctx, base, A_SET_FADER, 1, 0); gcall(ctx, base, A_FADE_OUT, 1, 30); }
/* fading to black: 1 once black. The fader only starts a fade-out from fully shown, so one
   asked for during a fade-in doesn't happen: ask again once the fade-in is over. (Before,
   pressing a button during a fade-in left the screen waiting for black forever, input off.) */
static int screen_black(ppc_context *ctx, uint8_t *base) {
    int st = fader_status(ctx, base);
    if (st == FADER_OPAQUE) return 1;
    if (st == FADER_HIDDEN) fade_out(ctx, base);
    return 0;
}
static void fade_in(ppc_context *ctx, uint8_t *base) { gcall(ctx, base, A_SET_FADER, 1, 0); gcall(ctx, base, A_FADE_IN, 1, 30); }
static void play_se(ppc_context *ctx, uint8_t *base, uint32_t id) { gcall(ctx, base, A_START_SYSTEM_SE, 3, R32(G_SND_AUDIO_MGR), id, 1); }
/* dInfo_c::setPlyConnectStage: the first four players' entries are where the game had them */
static uint32_t connect_stage_addr(uint32_t player) { uint32_t info = R32(G_INFO); return player < 4 ? info + 900 + 4 * player : info + 2892 + 4 * player; }

/* ---- what the setup scene is showing ---- */
enum { PH_GAME, PH_FILE, PH_CHAR };
static const char *ph_name[] = {"Game Select", "File Select", "character select"};
static int phase = PH_GAME;              /* the screen in use */
static int pending = -1;                 /* the screen to set up once the screen is black */
static int fs_fade_in_wait;              /* file select set up behind black: fade in when it's ready */
static int char_fade_in_wait;            /* character select set up behind black: fade in when it's in place */
static uint64_t wait_start;
static int np_exit_wait;                 /* character select: leaving, fading out first */
static int np_back_wait;                 /* character select: 1 pressed, fading out first */
static uint32_t np_buttons;              /* singleplayer: the select whose Start Game button shows (0: none) */
static struct { uint8_t ind[8][2], tdp[8], flag; } np_snap;   /* what showing the button changed */
static uint32_t cb_rejoin;               /* singleplayer: the card whose character was taken back */
static uint32_t gs_scene;                /* the dScGameSetup_c, while the scene runs */

static int in_setup(void) { return R16(G_NOW_SCENE) == SCENE_GAME_SETUP; }
static int is_multi(void) { return !menu_singleplayer(); }
static void set_phase(int p) {
    if (p != phase) rt_log("setup", "%s", ph_name[p]);
    phase = p;
}
static void reset_if_left(void) {
    if (!in_setup() && (gs_scene || phase != PH_GAME || pending >= 0)) {
        gs_scene = 0; phase = PH_GAME; pending = -1; fs_fade_in_wait = char_fade_in_wait = np_exit_wait = np_back_wait = 0;
        np_buttons = 0; cb_rejoin = 0;
    }
}

/* ======================================================= dScGameSetup_c (8-player version, native) */

/* Phase_CreateLayoutManagers: the screens. Profile numbers differ between game revisions. */
void hle_mod_fn_2_1AA4E0(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3];
    gs_scene = self; phase = PH_GAME; pending = -1;                       /* a new setup scene starts at Game Select */
    fs_fade_in_wait = char_fade_in_wait = np_exit_wait = np_back_wait = 0;
    rt_log("setup", "scene %08X: Game Select (%s)", self, is_multi() ? "multiplayer" : "singleplayer");
    int ver = (int)R32(MOD_GAME_VERSION), lt6 = ver < 6, lt8 = ver < 8, off = lt6 ? 0 : lt8 ? 2 : 4;
    uint32_t np = gcall(ctx, base, A_CREATE_CHILD, 4, lt8 ? 694u : 696u, self, 0, 0);
    W32(self + GS_NUMPY, np);
    for (uint32_t cc = 0; cc < R32(np + NP_CC_COUNT); cc++) {
        W32(np + NP_CC_BASE + 4 * cc, gcall(ctx, base, A_CREATE_CHILD, 4, 735u + off, self, cc, 0));
        W32(np + NP_CC_CONTENTS + 4 * cc, gcall(ctx, base, A_CREATE_CHILD, 4, 736u + off, self, cc, 0));
        W32(np + NP_CC_ARROW + 4 * cc, gcall(ctx, base, A_CREATE_CHILD, 4, 737u + off, self, cc, 0));
        W32(np + NP_CC_INDICATOR + 4 * cc, gcall(ctx, base, A_CREATE_CHILD, 4, 738u + off, self, cc, 0));
    }
    W32(self + GS_SELPLY, gcall(ctx, base, A_CREATE_CHILD, 4, lt8 ? 700u : 702u, self, 0, 0));
    W32(self + GS_EASY_PAIRING, gcall(ctx, base, A_CREATE_CHILD, 4, lt8 ? 697u : 699u, self, 0, 0));
    W32(self + GS_SEQUENCE_BG, gcall(ctx, base, A_CREATE_CHILD, 4, 721u + off, self, 0, 0));
    uint32_t fs = gcall(ctx, base, A_CREATE_CHILD, 4, 725u + off, self, R32(self + 4), 0);
    W32(self + GS_FSEL, fs);
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t df = gcall(ctx, base, A_CREATE_CHILD, 4, 724u + off, self, i, 0);
        W32(self + GS_DATEFILE + 4 * i, df); W32(fs + FS_DATEFILE + 4 * i, df);
    }
    uint32_t iw = gcall(ctx, base, A_CREATE_CHILD, 4, 718u + off, self, 0, 0);
    W32(self + GS_INFO_WINDOW, iw); W32(fs + FS_INFO_WINDOW, iw);
    ctx->r[3] = 1;
}

/* Phase_Create2DPlayer: one character model per call, all eight */
void hle_mod_fn_2_1AA710(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3], idx = R32(self + GS_CREATE_IDX);
    uint32_t pl = gcall(ctx, base, A_CONSTRUCT_ACTOR, 5, (int)R32(MOD_GAME_VERSION) < 8 ? 698u : 700u, self, idx, 0, 0);
    if (!pl) { ctx->r[3] = 0; return; }
    if (idx < 4) W32(self + GS_2DPLAYER + 4 * idx, pl);
    W32(R32(self + GS_NUMPY) + NP_2DPLAYER + 4 * idx, pl);
    if (++idx < 8) { W32(self + GS_CREATE_IDX, idx); ctx->r[3] = 0; return; }
    W32(self + GS_CREATE_IDX, 0); ctx->r[3] = 1;
}

/* the character select comes up behind black: as the module's "Select Players" answer did,
   minus that screen (1 player in Singleplayer, 8 in Multiplayer) */
static void char_select_open(ppc_context *ctx, uint8_t *base, uint32_t self) {
    uint32_t np = R32(self + GS_NUMPY);
    for (uint32_t i = 0; i < 8; i++) W32(connect_stage_addr(i), 0);       /* PlyConnectStage OFF */
    W8(np + NP_ACTIVE, 1);
    W32(np + NP_PLAYER_COUNT, is_multi() ? 8 : 1);
    change_state(ctx, base, self + GS_STATE, ST_GS_CONNECTION_CHECK);
    char_fade_in_wait = 1; wait_start = g_frames_presented;
}
/* the file select comes up behind black (Game Select or File Select) */
static void file_select_open(ppc_context *ctx, uint8_t *base, uint32_t self) {
    uint32_t fs = R32(self + GS_FSEL);
    W8(self + GS_IN_PLAYER_SELECT, 0);
    change_state(ctx, base, self + GS_STATE, ST_GS_FILE_SELECT);          /* (switches it on again) */
    /* the cards start over (what their "reset" request does - asked for by the fade-out's end,
       but they only act on it while shown, which they no longer are) */
    for (int i = 0; i < 3; i++) {
        uint32_t df = R32(fs + FS_DATEFILE + 4 * i);
        change_state(ctx, base, df + DF_STATE, ST_DF_INITIAL_SETUP);
        W8(df + DF_ACTIVE, 0); W32(df + DF_REQUEST, 0);
    }
    change_state(ctx, base, fs + FS_STATE, ST_FS_START_WAIT);             /* (what its GameStartWait does after a fade; clears the choice) */
    int last = (int8_t)R8(R32(G_SAVE_MNG) + SM_LAST_FILE);               /* the cursor starts on the last file played (Game Select: the middle card) */
    int cur = phase == PH_GAME ? 1 : (last >= 0 && last < 3) ? last : 0;
    W32(fs + FS_CHOICE, (uint32_t)cur); W32(fs + FS_LAST_CHOICE, (uint32_t)cur);
    fs_fade_in_wait = 1; wait_start = g_frames_presented;
}

/* executeState_FileSelect */
void hle_mod_fn_2_1AAFE0(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3], fs = R32(self + GS_FSEL);
    gs_scene = self;
    fader_status(ctx, base);
    if (phase == PH_GAME) W32(fs + FS_CHOICE, 1);                       /* Game Select: the one card is the middle one */
    gcall(ctx, base, A_DATEFILE_ANIME_OUT, 1, self);
    if (fs_fade_in_wait) {                                                /* set up behind black: show it once it's ready */
        if (state_is(ctx, base, fs + FS_STATE, "SelectWait") || g_frames_presented - wait_start > 120) {
            rt_log("setup", "%s ready after %llu frames (frame %llu)", ph_name[phase], (unsigned long long)(g_frames_presented - wait_start), (unsigned long long)g_frames_presented);
            fade_in(ctx, base); fs_fade_in_wait = 0;
        }
    }
    if (!R8(fs + FS_CHOICE_DONE)) return;
    int back = R8(fs + FS_CHOICE_BACK);
    if (phase == PH_GAME) {
        if (back) {                                                       /* Game Select -> title */
            gcall(ctx, base, A_START_TITLE, 2, 0, 0);
            change_state(ctx, base, self + GS_STATE, ST_GS_NEXT_SCENE_WAIT);
            return;
        }
        pending = PH_FILE;                                                /* NSMBW (the only game for now) */
        W8(self + GS_IN_PLAYER_SELECT, 0);
        change_state(ctx, base, self + GS_STATE, ST_GS_FADE_OUT);
        return;
    }
    if (back) {                                                           /* File Select -> Game Select */
        pending = PH_GAME;
        W8(self + GS_IN_PLAYER_SELECT, 0);
        change_state(ctx, base, self + GS_STATE, ST_GS_FADE_OUT);
        return;
    }
    W32(R32(self + GS_SELPLY) + SP_BUTTON, 0);
    W8(self + GS_IN_PLAYER_SELECT, 1);
    int choice = (int)R32(fs + FS_CHOICE);
    if (choice < 3) {                                                     /* a save file */
        if (R8(fs + FS_CHOICE_TEMP)) gcall(ctx, base, A_LOAD_QUICK_SAVE, 2, R32(G_SAVE_MNG), (uint32_t)choice & 0xFF);
        gcall(ctx, base, A_INIT_LOAD_GAME, 2, R32(G_SAVE_MNG), (uint32_t)(int8_t)choice);
    } else {                                                              /* Free-for-All / Coin Battle */
        for (int i = 0; i < 8; i++) W32(MOD_PY_REST + 4 * i, 5);
        gcall(ctx, base, A_CHECK_BONUS_NOCAP, 0);
        uint32_t f = R32(G_GAME_FLAG) & ~(uint32_t)(GF_MULTI_MODE | GF_FREE_FOR_ALL | GF_COIN_BATTLE);
        W32(G_GAME_FLAG, f | GF_MULTI_MODE | (choice == 3 ? GF_FREE_FOR_ALL : GF_COIN_BATTLE));
        W8(MOD_OVERLAY_RES, 0);
    }
    pending = PH_CHAR;
    change_state(ctx, base, self + GS_STATE, ST_GS_FADE_OUT);
}

/* finalizeState_FileSelect: default characters */
void hle_mod_fn_2_1AB140(ppc_context *ctx, uint8_t *base) {
    (void)ctx; (void)base;
    for (int i = 0; i < 8; i++) W32(MOD_PY_TYPE + 4 * i, R32(MOD_DEFAULT_TYPES + 4 * i));
}

/* executeState_FadeOut: once black, set up the next screen */
void hle_fn_2_1AB1B0(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3];
    gs_scene = self;
    if (!screen_black(ctx, base)) return;
    if (pending == PH_CHAR || (pending < 0 && R8(self + GS_IN_PLAYER_SELECT))) {
        pending = -1; set_phase(PH_CHAR);
        char_select_open(ctx, base, self);
        return;
    }
    if (pending >= 0) { set_phase(pending); pending = -1; }
    file_select_open(ctx, base, self);
}

/* executeState_FadeIn (no longer used by the flow above; kept equivalent) */
void hle_fn_2_1AB280(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3];
    gs_scene = self;
    if (fader_status(ctx, base) != FADER_HIDDEN) return;
    if (R8(self + GS_IN_PLAYER_SELECT)) char_select_open(ctx, base, self);
    else change_state(ctx, base, self + GS_STATE, ST_GS_FILE_SELECT);
}

/* executeState_StartMember: answer of the old "Select Players" screen (not shown any more) */
void hle_mod_fn_2_1AB360(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3], sp = R32(self + GS_SELPLY);
    if (!R8(sp + SP_DECIDED)) return;
    int dir = (int)R32(sp + SP_DIRECTION);
    if (dir == -1) { W8(self + GS_IN_PLAYER_SELECT, 0); change_state(ctx, base, self + GS_STATE, ST_GS_FADE_OUT); return; }
    if (dir != 1) return;
    uint32_t np = R32(self + GS_NUMPY);
    for (uint32_t i = 0; i < 8; i++) W32(connect_stage_addr(i), 0);
    W8(np + NP_ACTIVE, 1);
    W32(np + NP_PLAYER_COUNT, R32(sp + SP_BUTTON) + 1);
    change_state(ctx, base, self + GS_STATE, ST_GS_CONNECTION_CHECK);
}

/* initializeState_ConnectionCheck: no star power left over from a versus game; music */
void hle_mod_fn_2_1AB470(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3];
    if (R32(G_GAME_FLAG) & GF_MULTI_MODE) {
        uint32_t np = R32(self + GS_NUMPY);
        for (int i = 0; i < 8; i++) {
            uint32_t type = R32(MOD_PY_TYPE + 4 * i);
            W32(MOD_PY_MODE + 4 * type, 0);
            uint32_t mdl = R32(R32(R32(np + NP_2DPLAYER + 4 * type) + 296) + 4);   /* mp2DPlayer[type]->mModelMng->mModel */
            if (R32(mdl + 380) & 0x100) gcall(ctx, base, VFN(mdl, 68), 1, mdl);   /* offStarAnm */
            if (R32(mdl + 380) & 0x200) gcall(ctx, base, VFN(mdl, 76), 1, mdl);   /* offStarEffect */
            uint32_t ci = MOD_PY_CREATE_ITEM + 4 * type;
            if (R32(ci) & 1) W32(ci, R32(ci) & ~1u);                     /* no STAR_POWER */
        }
    }
    gcall(ctx, base, A_SET_SETUP_TRACK, 2, R32(G_SND_SCENE_MGR), 2);
}

/* the character select is up and settled (it came in behind black) */
static int char_select_settled(uint32_t np) {
    if (!R8(np + NP_CANCEL_OK)) return 0;
    for (uint32_t cc = 0; cc < R32(np + NP_PLAYER_COUNT) && cc < 8; cc++) if (R8(R32(np + NP_CC_BASE + 4 * cc) + CB_BUSY)) return 0;
    return 1;
}
static int plus_pressed(void) {
    uint32_t core = R32(R32(G_GAME_KEY) + 4 + 4 * R32(G_CUR_CORE));
    return core && (R32(core + 28) & 0x10);
}

/* executeState_ConnectionCheck: the character select is open */
void hle_fn_2_1AB570(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3], np = R32(self + GS_NUMPY);
    gs_scene = self;
    fader_status(ctx, base);
    if (char_fade_in_wait) {
        if (char_select_settled(np) || g_frames_presented - wait_start > 240) {
            rt_log("setup", "character select (%u players) ready after %llu frames (frame %llu)", R32(np + NP_PLAYER_COUNT), (unsigned long long)(g_frames_presented - wait_start), (unsigned long long)g_frames_presented);
            fade_in(ctx, base); char_fade_in_wait = 0;
        }
        return;
    }
    if (R8(np + NP_CANCEL_OK)) {
        if (plus_pressed() && !menu_controls_is_open()) {                /* "Settings +": the Controls screen (was: pair a remote) */
            play_se(ctx, base, SE_SYS_DECIDE);
            menu_controls_open();
        }
        return;
    }
    if (R8(np + NP_ACTIVE)) return;
    if (R32(np + NP_ON_BUTTON) == 0) { change_state(ctx, base, self + GS_STATE, ST_GS_LOW_BATTERY_CHECK); return; }   /* Start Game */
    set_phase(PH_FILE);                                                   /* Back: the character select has gone (behind black) */
    file_select_open(ctx, base, self);
}

/* initializeState_EasyPairingWait / finalizeState_EasyPairingWait (pairing a remote; not reached from the menus now) */
void hle_mod_fn_2_1AB620(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3];
    gcall(ctx, base, MFN_SET_EASY_PAIRING_WAIT, 2, R32(self + GS_NUMPY), 1);
    W8(R32(self + GS_EASY_PAIRING) + SP_EASY_PAIRING_FLAG, 1);
}
void hle_mod_fn_2_1AB6D0(ppc_context *ctx, uint8_t *base) {
    gcall(ctx, base, MFN_SET_EASY_PAIRING_WAIT, 2, R32(ctx->r[3] + GS_NUMPY), 0);
}

/* finalizeState_VoiceEndWait: on to the world map (or the opening movie for a new save) */
void hle_mod_fn_2_1AB9F0(ppc_context *ctx, uint8_t *base) {
    uint32_t save = gcall(ctx, base, A_GET_SAVE_GAME, 2, R32(G_SAVE_MNG), (uint32_t)-1);
    if ((R8(save + 2) & 1) && !R8(G_DISABLE_OPENING_MOVIE)) gcall(ctx, base, A_DO_SCENE_CHANGE, 3, 8, 0, 0);   /* MOVIE */
    else gcall(ctx, base, A_DO_SCENE_CHANGE, 3, 3, gcall(ctx, base, A_WMAP_BOOT_PARAM, 0), 0);                /* WORLD_MAP */
}

/* ======================================================= dNumberOfPeopleChange_c (in part) */

/* checkCancel (module version, unchanged) */
static int np_check_cancel(ppc_context *ctx, uint8_t *base, uint32_t np) {
    int p1 = R8(R32(np + NP_CC_BASE) + CB_DECIDED);
    if (R16(G_NOW_SCENE) == SCENE_WORLD_MAP) {
        if (!p1) return 0;
        uint32_t core = R32(R32(G_GAME_KEY) + 4 + 4 * R32(G_CUR_CORE));
        return gcall(ctx, base, VFN(core, 32), 2, core, 0x900) != 0;    /* checkMenuConfirm */
    }
    if (p1) return 0;                                                     /* 1 first takes back player 1's choice */
    return gcall(ctx, base, A_CHK_CANCEL, 1, 0) != 0;
}
static int base_joined(ppc_context *ctx, uint8_t *base, uint32_t b) { return !state_is(ctx, base, b + CB_STATE, "ConnectWait"); }
/* leave the character select: in the setup scene the screen fades out first, everything in place */
static int np_leave(ppc_context *ctx, uint8_t *base, uint32_t np, int on_button, uint32_t state) {
    if (in_setup()) {
        if (!np_exit_wait) { np_exit_wait = 1; fade_out(ctx, base); return 0; }
        if (!screen_black(ctx, base)) return 0;
        np_exit_wait = 0;
    }
    if (on_button >= 0) W32(np + NP_ON_BUTTON, (uint32_t)on_button);
    for (uint32_t cc = 0; cc < R32(np + NP_PLAYER_COUNT) && cc < 8; cc++) W8(R32(np + NP_CC_BASE + 4 * cc) + CB_EXIT, 1);
    change_state(ctx, base, np + NP_STATE, state);
    return 1;
}

/* executeState_NowEntrantRecruit: players pick their characters */
void hle_mod_fn_2_33DA0(ppc_context *ctx, uint8_t *base) {
    uint32_t np = ctx->r[3];
    /* a Classic Controller can be split from its remote with - (module feature) */
    uint32_t rm = R32(G_REMOCON_MNG);
    for (uint32_t plr = 0; plr < 8; plr++) {
        uint32_t con = R32(rm + 4 + 4 * plr), sm = R32(con + 144);
        if (!gcall(ctx, base, VFN(sm, 16), 2, sm, G_SETUP_STATE_ID)) continue;          /* isSetup */
        if ((R32(con + 12) & ~3u) == 8 || R32(con + 16) != 4) continue;                  /* split already / not Classic */
        uint32_t core = R32(R32(G_GAME_KEY) + 4 + 4 * plr);
        if (R8(core + 30) & 0x10) gcall(ctx, base, MFN_SPLIT_EXTENSION, 1, con);         /* - pressed */
    }
    if (np_back_wait) {                                                   /* Back pressed: fading out */
        if (np_leave(ctx, base, np, 1, ST_NP_EXIT_ANIME_END_CHECK)) np_back_wait = 0;
        return;
    }
    uint32_t count = R32(np + NP_PLAYER_COUNT); if (count > 8) count = 8;
    for (uint32_t cc = 0; cc < count; cc++) if (R8(R32(np + NP_CC_BASE + 4 * cc) + CB_BUSY)) return;
    int wmap = R16(G_NOW_SCENE) == SCENE_WORLD_MAP;
    if (np_check_cancel(ctx, base, np)) {
        play_se(ctx, base, wmap ? SE_SYS_DECIDE : SE_SYS_BACK);
        if (wmap) { np_leave(ctx, base, np, 0, ST_NP_EXIT_ANIME_END_CHECK); return; }
        np_back_wait = 1;
        if (np_leave(ctx, base, np, 1, ST_NP_EXIT_ANIME_END_CHECK)) np_back_wait = 0;
        return;
    }
    if (wmap) return;
    if (in_setup() && is_multi() && count == 8) {
        /* Multiplayer: Start Game once players 1 and 2 have chosen and nobody who joined is still choosing */
        for (uint32_t cc = 0; cc < count; cc++) {
            uint32_t b = R32(np + NP_CC_BASE + 4 * cc);
            if (R8(b + CB_DECIDED)) continue;
            if (cc < 2 || base_joined(ctx, base, b)) return;
        }
    } else {
        for (uint32_t cc = 0; cc < count; cc++) if (!R8(R32(np + NP_CC_BASE + 4 * cc) + CB_DECIDED)) return;
    }
    /* everyone chose: the Start Game button comes (remember what that changes, for taking a
       choice back - singleplayer, below) */
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t ind = R32(np + NP_CC_INDICATOR + 4 * i), tdp = R32(np + NP_2DPLAYER + 4 * i);
        for (int k = 0; k < 2; k++) np_snap.ind[i][k] = ind && R32(ind + NP_IND_PANES + 4 * k) ? R8(R32(ind + NP_IND_PANES + 4 * k) + 0xBB) : 0;
        np_snap.tdp[i] = tdp ? R8(tdp + TDP_FLAG) : 0;
    }
    np_snap.flag = R8(np + NP_BUTTONS_FLAG);
    np_buttons = in_setup() && !is_multi() ? np : 0;
    for (uint32_t cc = 0; cc < count; cc++) W8(R32(np + NP_CC_BASE + 4 * cc) + CB_EXIT, 1);
    change_state(ctx, base, np + NP_STATE, ST_NP_INFO_WINDOW_EXIT_WAIT);
}

/* dCharacterChangeSelectBase_c::executeState_SelectStartWait ("Join": 2 brings the card): a
   card whose character was taken back (singleplayer, above) comes back by itself */
void hle_fn_2_2DF0(ppc_context *ctx, uint8_t *base) {
    uint32_t b = ctx->r[3];
    if (b == cb_rejoin && in_setup() && !is_multi()) {
        cb_rejoin = 0;
        change_state(ctx, base, b + CB_STATE, ST_CB_BUTTON_EXIT_ANIME);
        return;
    }
    orig_fn_2_2DF0(ctx, base);
}

/* executeState_ButtonSelect. Singleplayer: only Start Game (centred; the Back button is gone -
   "Back 1" shows instead, see gs_layout_draw), and 1 takes the character back: the select goes
   back to choosing (1 again: back to the File Select, NowEntrantRecruit). Multiplayer: the
   module's (Start Game / Back). */
void hle_mod_fn_2_343F0(ppc_context *ctx, uint8_t *base) {
    uint32_t np = ctx->r[3];
    if (!in_setup() || is_multi() || np != np_buttons) { orig_mod_fn_2_343F0(ctx, base); return; }
    if (gcall(ctx, base, A_CHK_CANCEL, 1, 0)) {
        uint32_t count = R32(np + NP_PLAYER_COUNT); if (count > 8) count = 8;
        /* the button goes (its animations back to their first frame, as before it came in);
           what it hid comes back */
        for (uint32_t g = NPA_IN_FIRST; g <= NPA_ON_LAST; g++) gcall(ctx, base, A_ANIM_RESET, 3, np + NP_LAYOUT, g, 0);
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t ind = R32(np + NP_CC_INDICATOR + 4 * i), tdp = R32(np + NP_2DPLAYER + 4 * i);
            for (int k = 0; k < 2; k++) if (ind && R32(ind + NP_IND_PANES + 4 * k)) W8(R32(ind + NP_IND_PANES + 4 * k) + 0xBB, np_snap.ind[i][k]);
            if (tdp) W8(tdp + TDP_FLAG, np_snap.tdp[i]);
        }
        W8(np + NP_BUTTONS_FLAG, np_snap.flag);
        /* player 1 takes the choice back, as the card does when 1 is pressed on it (PlayerDisp) */
        for (uint32_t cc = 0; cc < count; cc++) W8(R32(np + NP_CC_BASE + 4 * cc) + CB_EXIT, 0);
        uint32_t b = R32(np + NP_CC_BASE), ec = R32(b + CB_ENTER_COUNT);
        play_se(ctx, base, SE_SYS_BACK);
        if (ec) W32(ec, (int)R32(ec) > 1 ? R32(ec) - 1 : 0);
        chars_keep_choice(0);                                             /* the card comes back with it */
        cb_rejoin = b;
        change_state(ctx, base, b + CB_STATE, ST_CB_PLAYER_EXIT_WAIT);
        np_buttons = 0;
        change_state(ctx, base, np + NP_STATE, ST_NP_NOW_ENTRANT_RECRUIT);
        rt_log("setup", "character select: player 1 takes the character back");
        return;
    }
    /* left / right: nothing to move to */
    W32(np + NP_ON_BUTTON, 0);
    uint32_t core = R32(R32(G_GAME_KEY) + 4 + 4 * R32(G_CUR_CORE)), trig = core ? R32(core + 28) : 0;
    if (core) W32(core + 28, trig & ~0xCu);
    orig_mod_fn_2_343F0(ctx, base);
    if (core) W32(core + 28, R32(core + 28) | (trig & 0xCu));
}

/* executeState_ButtonDecision: Start Game / Back was pressed; after its animation the screen goes */
void hle_mod_fn_2_345C0(ppc_context *ctx, uint8_t *base) {
    uint32_t np = ctx->r[3];
    if (gcall(ctx, base, A_IS_ANIM_ON, 2, np + NP_LAYOUT, (uint32_t)-1)) return;
    if (in_setup()) {                                                     /* fade out with the screen in place */
        if (!np_exit_wait) { np_exit_wait = 1; fade_out(ctx, base); return; }
        if (!screen_black(ctx, base)) return;
        np_exit_wait = 0;
    }
    change_state(ctx, base, np + NP_STATE, ST_NP_EXIT_ANIME_END_CHECK);
}

/* ======================================================= input */
/* game input while the setup scene changes screens is dropped; Game Select takes only 2/A
   (choose) and 1/B (back) */
enum { B_LEFT = 0x0001, B_RIGHT = 0x0002, B_DOWN = 0x0004, B_UP = 0x0008, B_PLUS = 0x0010, B_MINUS = 0x1000 };
static int fs_screen_live;               /* the file select layout was drawn lately */
static uint64_t fs_drawn_frame;
uint32_t gs_filter_buttons(uint32_t b) {
    reset_if_left();
    if (!in_setup() || !gs_scene) return b;
    if (pending >= 0 || fs_fade_in_wait || char_fade_in_wait || np_exit_wait || np_back_wait || fader_seen != FADER_HIDDEN) return b & ~0xFFFFu;
    fs_screen_live = g_frames_presented - fs_drawn_frame <= 3;
    if (phase == PH_GAME && fs_screen_live) b &= ~(uint32_t)(B_LEFT | B_RIGHT | B_UP | B_DOWN | B_PLUS | B_MINUS);
    else if (phase == PH_FILE && fs_screen_live && menu_singleplayer()) b &= ~(uint32_t)B_LEFT;   /* (down, sideways: the hidden versus buttons) */
    return b;
}
/* the character select is up and settled (rt_menu.c: the host starts an online game here) */
int gs_char_select_ready(void) { return in_setup() && gs_scene && phase == PH_CHAR && pending < 0 && !char_fade_in_wait && !np_exit_wait && !np_back_wait; }
int gs_busy(void) { return in_setup() && (pending >= 0 || fs_fade_in_wait || char_fade_in_wait || np_exit_wait || np_back_wait); }

/* ======================================================= layouts */
static float rdf(uint32_t a) { uint32_t u = R32(a); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); W32(a, u); }

/* text boxes: +0xD8 buffer (UTF-16BE), +0xFC buffer bytes, +0xFE length. The game's text is
   kept so it can be put back (Game Select -> File Select). */
#define MAXSAVE 24
static struct { uint32_t tb; uint16_t text[80]; int n; uint32_t size[2]; } saved[MAXSAVE]; static int nsaved; static uint32_t saved_scene;
static int text_read(uint32_t tb, uint16_t *out, int max) {
    uint32_t buf = R32(tb + 0xD8); int n = R16(tb + 0xFE);
    if (!buf || n > max) n = buf ? max : 0;
    for (int i = 0; i < n; i++) out[i] = R16(buf + 2 * i);
    return n;
}
static void text_write(uint32_t tb, const uint16_t *s, int n) {
    uint32_t buf = R32(tb + 0xD8); int cap = R16(tb + 0xFC) / 2 - 1;
    if (!buf || cap <= 0) return;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) W16(buf + 2 * i, s[i]);
    W16(buf + 2 * n, 0); W16(tb + 0xFE, (uint16_t)n);
}
static void saves_check_scene(void) { if (saved_scene != gs_scene) { nsaved = 0; saved_scene = gs_scene; } }
/* show s in the text box; keep_tail keeps the game's text from its first button glyph on;
   scale (if not 0) changes the font size from the game's */
static void text_set_scaled(uint32_t tb, const char *s, int keep_tail, float scale) {
    if (!tb) return;
    saves_check_scene();
    int k; for (k = 0; k < nsaved && saved[k].tb != tb; k++) ;
    if (k == nsaved) { if (nsaved == MAXSAVE) return; saved[k].tb = tb; saved[k].n = text_read(tb, saved[k].text, 80);
                       saved[k].size[0] = R32(tb + 0xE8); saved[k].size[1] = R32(tb + 0xEC); nsaved++; }
    if (scale > 0) { float f; memcpy(&f, &saved[k].size[0], 4); wrf(tb + 0xE8, f * scale); memcpy(&f, &saved[k].size[1], 4); wrf(tb + 0xEC, f * scale); }
    uint16_t t[96]; int n = 0;
    for (; *s && n < 80; s++) t[n++] = (uint8_t)*s;
    if (keep_tail) {
        int i; for (i = 0; i < saved[k].n && saved[k].text[i] >= 0x20 && saved[k].text[i] < 0x7F; i++) ;
        for (; i < saved[k].n && n < 95; i++) t[n++] = saved[k].text[i];
    }
    uint16_t cur[96]; int cn = text_read(tb, cur, 96);
    if (cn != n || memcmp(cur, t, 2 * n)) text_write(tb, t, n);
}
static void text_set(uint32_t tb, const char *s, int keep_tail) { text_set_scaled(tb, s, keep_tail, 0); }
static void text_restore(uint32_t tb) {
    if (!tb || saved_scene != gs_scene) return;
    for (int k = 0; k < nsaved; k++) if (saved[k].tb == tb) {
        text_write(tb, saved[k].text, saved[k].n); W32(tb + 0xE8, saved[k].size[0]); W32(tb + 0xEC, saved[k].size[1]);
        saved[k] = saved[--nsaved]; return;
    }
}
/* visibility changed from what the game set; put back when no longer wanted */
#define MAXVIS 32
static struct { uint32_t pane; uint8_t flags; } vsaved[MAXVIS]; static int nvsaved; static uint32_t vsaved_scene;
static void vis_set(uint32_t pane, int v) {
    if (!pane) return;
    if (vsaved_scene != gs_scene) { nvsaved = 0; vsaved_scene = gs_scene; }
    int k; for (k = 0; k < nvsaved && vsaved[k].pane != pane; k++) ;
    if (k == nvsaved) { if (nvsaved == MAXVIS) return; vsaved[k].pane = pane; vsaved[k].flags = R8(pane + 0xBB); nvsaved++; }
    lyt_set_visible(pane, v);
}
static void vis_restore(uint32_t pane) {
    if (!pane || vsaved_scene != gs_scene) return;
    for (int k = 0; k < nvsaved; k++) if (vsaved[k].pane == pane) { W8(pane + 0xBB, vsaved[k].flags); vsaved[k] = vsaved[--nvsaved]; return; }
}
/* hide (or show) a pane while cond holds, as the game had it otherwise */
static void show_if(uint32_t root, const char *name, int cond, int v) { uint32_t p = lyt_find_pane(root, name); if (cond) vis_set(p, v); else vis_restore(p); }

/* a pane moved from where the game put it (the game may set it again: then that is the new base) */
#define MAXSHIFT 8
static struct { uint32_t pane; int axis; float base, written; } shifts[MAXSHIFT]; static int nshift; static uint32_t shift_scene;
static void pane_shift(uint32_t pane, int axis, float d) {
    if (!pane) return;
    if (shift_scene != gs_scene) { nshift = 0; shift_scene = gs_scene; }
    uint32_t a = pane + 0x2C + 4 * axis;                                  /* mTranslate.x / .y */
    int k; for (k = 0; k < nshift && (shifts[k].pane != pane || shifts[k].axis != axis); k++) ;
    float v = rdf(a);
    if (k == nshift) { if (nshift == MAXSHIFT) return; nshift++; shifts[k].pane = pane; shifts[k].axis = axis; shifts[k].base = v; }
    else if (v != shifts[k].written) shifts[k].base = v;
    shifts[k].written = shifts[k].base + d;
    wrf(a, shifts[k].written);
}


/* ======================================================= game portraits
 * Game Select: each game's card shows its portrait, ui/graphics/gameportrait/<id>.png (any size,
 * PNG; transparent pixels stay transparent). The card takes the picture's shape: as tall as
 * before, as wide as the picture (its white border stays 5 px all round), and the game's name
 * shows only when there is no picture. The picture is scaled to fit 512 x 512 and kept as a GX
 * CMPR (DXT1) texture in our guest memory, so a few games fit. */
typedef struct { const char *id, *label; } game_t;
static const game_t GAMES[] = { {"nsmbw", "NSMBW"} };       /* (level and map packs come later) */
#define NGAMES  (int)(sizeof GAMES / sizeof GAMES[0])
#define GP_MEM      0x81870000u                             /* free: past the module's loader scratch, before rt_retro's */
#define GP_MEM_END  0x81900000u
#define GP_MAX      512
typedef struct { int state; uint32_t tex; int tw, th; float aspect; } gport_t;   /* state 0 untried, 1 ok, -1 none */
static gport_t gports[NGAMES];
static uint32_t gp_next = GP_MEM;
static int gp_borrowed;
void boot_screen_mem_lost(void);

/* area-average resample of 0xAARRGGBB to dw x dh (straight alpha: colour weighted by alpha) */
static float *gp_resample(const uint32_t *px, int w, int h, int dw, int dh) {
    float *row = calloc((size_t)dw * h * 4, sizeof(float)), *out = calloc((size_t)dw * dh * 4, sizeof(float));
    double sx = (double)w / dw, sy = (double)h / dh;
    for (int y = 0; y < h; y++) for (int x = 0; x < dw; x++) {      /* horizontal */
        double a0 = x * sx, a1 = a0 + sx, acc[4] = {0}, tw = 0;
        for (int i = (int)a0; i < w && i < a1; i++) {
            double wgt = (i + 1 < a1 ? i + 1 : a1) - (i > a0 ? i : a0); if (wgt <= 0) continue;
            uint32_t c = px[(size_t)y * w + i]; double al = (c >> 24) / 255.0;
            acc[0] += wgt * al * ((c >> 16) & 255); acc[1] += wgt * al * ((c >> 8) & 255); acc[2] += wgt * al * (c & 255); acc[3] += wgt * al; tw += wgt;
        }
        float *o = row + ((size_t)y * dw + x) * 4; for (int k = 0; k < 4; k++) o[k] = (float)(acc[k] / tw);
    }
    for (int x = 0; x < dw; x++) for (int y = 0; y < dh; y++) {     /* vertical */
        double a0 = y * sy, a1 = a0 + sy, acc[4] = {0}, tw = 0;
        for (int i = (int)a0; i < h && i < a1; i++) {
            double wgt = (i + 1 < a1 ? i + 1 : a1) - (i > a0 ? i : a0); if (wgt <= 0) continue;
            const float *r = row + ((size_t)i * dw + x) * 4; for (int k = 0; k < 4; k++) acc[k] += wgt * r[k]; tw += wgt;
        }
        float *o = out + ((size_t)y * dw + x) * 4;
        double al = acc[3] / tw;                                    /* back to straight colour */
        for (int k = 0; k < 3; k++) o[k] = al > 0 ? (float)(acc[k] / tw / al) : 0; o[3] = (float)(al * 255.0);
    }
    free(row); return out;
}
static uint16_t rgb565(const float c[3]) {
    int r = (int)(c[0] * 31 / 255 + 0.5f), g = (int)(c[1] * 63 / 255 + 0.5f), b = (int)(c[2] * 31 / 255 + 0.5f);
    r = r < 0 ? 0 : r > 31 ? 31 : r; g = g < 0 ? 0 : g > 63 ? 63 : g; b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint16_t)(r << 11 | g << 5 | b);
}
static void from565(uint16_t v, float c[3]) { int r = v >> 11, g = (v >> 5) & 63, b = v & 31; c[0] = (float)(r << 3 | r >> 2); c[1] = (float)(g << 2 | g >> 4); c[2] = (float)(b << 3 | b >> 2); }
/* the indices for endpoints c0, c1 (opaque: 4 colours, c0 > c1; with holes: 3 + transparent, c0 <= c1); returns the error */
static double dxt_fit(const float px[16][4], uint16_t c0, uint16_t c1, uint8_t idx[16]) {
    float p[4][3], a[3], b[3]; from565(c0, a); from565(c1, b);
    int four = c0 > c1;
    for (int k = 0; k < 3; k++) { p[0][k] = a[k]; p[1][k] = b[k];
        if (four) { p[2][k] = (2 * a[k] + b[k]) / 3; p[3][k] = (a[k] + 2 * b[k]) / 3; } else { p[2][k] = (a[k] + b[k]) / 2; p[3][k] = 0; } }
    double err = 0;
    for (int i = 0; i < 16; i++) {
        if (px[i][3] < 128) { idx[i] = 3; continue; }               /* (only asked for with holes: 3-colour mode) */
        int best = 0; double bd = 1e30;
        for (int j = 0; j < (four ? 4 : 3); j++) { double d = 0; for (int k = 0; k < 3; k++) { double e = px[i][k] - p[j][k]; d += e * e; } if (d < bd) { bd = d; best = j; } }
        idx[i] = (uint8_t)best; err += bd;
    }
    return err;
}
/* one 4x4 block -> 8 bytes of GX CMPR (big-endian colours; a row per byte, left pixel in the top bits) */
static void dxt_block(const float px[16][4], uint8_t out[8]) {
    int holes = 0, n = 0; float mean[3] = {0};
    for (int i = 0; i < 16; i++) { if (px[i][3] < 128) holes = 1; else { n++; for (int k = 0; k < 3; k++) mean[k] += px[i][k]; } }
    uint16_t c0 = 0, c1 = 0; uint8_t idx[16];
    if (!n) { memset(idx, 3, 16); goto write; }
    for (int k = 0; k < 3; k++) mean[k] /= n;
    {   double cov[6] = {0};                                        /* principal axis (power iteration) */
        for (int i = 0; i < 16; i++) if (px[i][3] >= 128) { double d[3]; for (int k = 0; k < 3; k++) d[k] = px[i][k] - mean[k];
            cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2]; cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2]; }
        double v[3] = {0.577, 0.577, 0.577};
        for (int it = 0; it < 8; it++) {
            double nv[3] = {cov[0] * v[0] + cov[1] * v[1] + cov[2] * v[2], cov[1] * v[0] + cov[3] * v[1] + cov[4] * v[2], cov[2] * v[0] + cov[4] * v[1] + cov[5] * v[2]};
            double l = sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]); if (l < 1e-9) break;
            for (int k = 0; k < 3; k++) v[k] = nv[k] / l;
        }
        double tmin = 1e30, tmax = -1e30;
        for (int i = 0; i < 16; i++) if (px[i][3] >= 128) { double t = 0; for (int k = 0; k < 3; k++) t += (px[i][k] - mean[k]) * v[k]; if (t < tmin) tmin = t; if (t > tmax) tmax = t; }
        float e0[3], e1[3]; for (int k = 0; k < 3; k++) { e0[k] = (float)(mean[k] + v[k] * tmax); e1[k] = (float)(mean[k] + v[k] * tmin); }
        c0 = rgb565(e0); c1 = rgb565(e1);
    }
    if (holes ? c0 > c1 : c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    if (!holes && c0 == c1) { memset(idx, 0, 16); goto write; }
    double err = dxt_fit(px, c0, c1, idx);
    if (!holes) {                                                   /* least squares on these indices, once */
        static const float wa[4] = {1, 0, 2.0f / 3, 1.0f / 3};
        double aa = 0, ab = 0, bb = 0, ax[3] = {0}, bx[3] = {0};
        for (int i = 0; i < 16; i++) { double x = wa[idx[i]], y = 1 - x; aa += x * x; ab += x * y; bb += y * y;
            for (int k = 0; k < 3; k++) { ax[k] += x * px[i][k]; bx[k] += y * px[i][k]; } }
        double det = aa * bb - ab * ab;
        if (fabs(det) > 1e-6) {
            float e0[3], e1[3];
            for (int k = 0; k < 3; k++) { e0[k] = (float)((ax[k] * bb - bx[k] * ab) / det); e1[k] = (float)((bx[k] * aa - ax[k] * ab) / det); }
            uint16_t n0 = rgb565(e0), n1 = rgb565(e1); if (n0 < n1) { uint16_t t = n0; n0 = n1; n1 = t; }
            uint8_t idx2[16];
            if (n0 != n1 && dxt_fit(px, n0, n1, idx2) < err) { c0 = n0; c1 = n1; memcpy(idx, idx2, 16); }
        }
    }
write:
    out[0] = (uint8_t)(c0 >> 8); out[1] = (uint8_t)c0; out[2] = (uint8_t)(c1 >> 8); out[3] = (uint8_t)c1;
    for (int r = 0; r < 4; r++) out[4 + r] = (uint8_t)(idx[r * 4] << 6 | idx[r * 4 + 1] << 4 | idx[r * 4 + 2] << 2 | idx[r * 4 + 3]);
}
static gport_t *game_portrait(int g) {
    if (g < 0 || g >= NGAMES) return NULL;
    gport_t *p = &gports[g];
    if (p->state) return p->state > 0 ? p : NULL;
    p->state = -1;
    char path[1200]; snprintf(path, sizeof path, "%s/ui/graphics/gameportrait/%s.png", g_root, GAMES[g].id);
    int w, h; uint32_t *px = ui_load_png(path, &w, &h);
    if (!px) return NULL;
    /* fit GP_MAX (never bigger than it is), in whole 8x8 tiles */
    double sc = (double)GP_MAX / (w > h ? w : h); if (sc > 1) sc = 1;
    int tw = (int)(w * sc / 8 + 0.5) * 8, th = (int)(h * sc / 8 + 0.5) * 8;
    if (tw < 8) tw = 8; if (th < 8) th = 8;
    if (gp_borrowed) { gp_borrowed = 0; boot_screen_mem_lost(); }
    uint32_t bytes = (uint32_t)tw * th / 2, at = (gp_next + 31) & ~31u;
    if (at + bytes > GP_MEM_END) { free(px); rt_log("setup", "%s: no room for another game picture", path); return NULL; }
    float *img = gp_resample(px, w, h, tw, th); free(px);
    uint8_t *dst = G(at);
    for (int ty = 0; ty < th; ty += 8) for (int tx = 0; tx < tw; tx += 8)
        for (int sb = 0; sb < 4; sb++) {                            /* (an 8x8 tile: its four 4x4 blocks, row by row) */
            int bx = tx + (sb & 1) * 4, by = ty + (sb >> 1) * 4; float blk[16][4];
            for (int i = 0; i < 16; i++) memcpy(blk[i], img + ((size_t)(by + (i >> 2)) * tw + bx + (i & 3)) * 4, 4 * sizeof(float));
            dxt_block(blk, dst); dst += 8;
        }
    free(img);
    p->tex = at; p->tw = tw; p->th = th; p->aspect = (float)w / h; p->state = 1;
    gp_next = at + bytes;
    rt_log("setup", "game picture %s: %dx%d, kept at %dx%d (%u KB)", GAMES[g].id, w, h, tw, th, bytes / 1024);
    return p;
}
/* the boot's keyboard picture (rt_bootscreen.c) borrows the whole region: it is shown long
   before Game Select; the first game picture after it takes the region back */
uint32_t gs_ui_mem_borrow(uint32_t bytes) {
    if (bytes > GP_MEM_END - GP_MEM) return 0;
    memset(gports, 0, sizeof gports); gp_next = GP_MEM; gp_borrowed = 1;
    return GP_MEM;
}
static int cur_game(void) { return 0; }                             /* (the only game for now) */

/* the card's picture pane shows the portrait: its texture and vertex colours (the card's
   blue-green gradient) are kept to put back, refreshed whenever they aren't ours (a new layout
   may reuse the pane's address) */
#define PIC_VTX      0xD8                                           /* nw4r::lyt::Picture: ut::Color[4] */
#define PANE_MAT     0x28
#define MAT_TEXMAP   0x40                                           /* the first TexMap: image, palette, u16 w, h, ..., u32 bits (format: top 4) */
static struct { uint32_t pane, texmap[7], vtx[4]; } gp_saved;
static int gp_ours(uint32_t image) { return image >= GP_MEM && image < GP_MEM_END; }
static void card_picture(uint32_t pane, gport_t *p) {
    uint32_t mat = pane ? R32(pane + PANE_MAT) : 0, tm = mat ? R32(mat + MAT_TEXMAP) : 0;
    if (!(tm >= 0x80000000u && tm < 0x94000000u)) return;
    if (gp_saved.pane != pane || !gp_ours(R32(tm))) {
        if (!p && gp_saved.pane != pane) return;                    /* (nothing of ours there) */
        if (!gp_ours(R32(tm))) { gp_saved.pane = pane; for (int i = 0; i < 7; i++) gp_saved.texmap[i] = R32(tm + 4 * i); for (int i = 0; i < 4; i++) gp_saved.vtx[i] = R32(pane + PIC_VTX + 4 * i); }
    }
    if (p) {
        W32(tm, p->tex); W16(tm + 8, (uint16_t)p->tw); W16(tm + 10, (uint16_t)p->th);
        W32(tm + 0x18, (gp_saved.texmap[6] & 0x0FFFFFFFu) | (14u << 28));       /* GX_TF_CMPR */
        for (int i = 0; i < 4; i++) W32(pane + PIC_VTX + 4 * i, 0xFFFFFFFFu);
    } else if (gp_ours(R32(tm))) {
        for (int i = 0; i < 7; i++) W32(tm + 4 * i, gp_saved.texmap[i]);
        for (int i = 0; i < 4; i++) W32(pane + PIC_VTX + 4 * i, gp_saved.vtx[i]);
    }
}
/* the card's width changed from what the game set (the game may set it again: then that is the
   new base); d = 0 puts it back */
static void cursor_note(uint32_t pane, float wide, float narrow);
#define MAXWIDTH 12
static struct { uint32_t pane; float base, written; } widths[MAXWIDTH]; static int nwidth; static uint32_t width_scene;
static void pane_narrow(uint32_t pane, float d) {
    if (!pane) return;
    if (width_scene != gs_scene) { nwidth = 0; width_scene = gs_scene; }
    int k; for (k = 0; k < nwidth && widths[k].pane != pane; k++) ;
    float v = rdf(pane + 0x4C);                                     /* mSize.width */
    if (d == 0) {
        if (k < nwidth) { if (v == widths[k].written) wrf(pane + 0x4C, widths[k].base); widths[k] = widths[--nwidth]; }
        return;
    }
    if (k == nwidth) { if (nwidth == MAXWIDTH) return; nwidth++; widths[k].pane = pane; widths[k].base = v; }
    else if (v != widths[k].written) widths[k].base = v;
    widths[k].written = widths[k].base - d;
    wrf(pane + 0x4C, widths[k].written);
    cursor_note(pane, widths[k].base, widths[k].written);
}
/* the select cursor (the four orange corners) took the card's size when it was set on it
   (dGameCom::SelectCursorSetup -> dSelectCursor_c: per cursor, 612 bytes from +0: +772 on,
   +773 the pane's name, +876 / +880 its width / height). A cursor on a card pane whose width
   we changed gets the width the card has now - both ways, as the game may set it up in the frame
   before our change or our putting back */
#define G_SELECT_CURSOR 0x8042A2C8u                                 /* dSelectCursor_c::m_instance (r13-21432) */
#define SC_STRIDE 612
#define SC_ON     772
#define SC_NAME   773
#define SC_WIDTH  876
static struct { char name[18]; float wide, narrow; } cfix[8]; static int ncfix;
static void cursor_note(uint32_t pane, float wide, float narrow) {
    char nm[18]; for (int i = 0; i < 17; i++) nm[i] = (char)R8(pane + 0xBC + i); nm[17] = 0;
    int k; for (k = 0; k < ncfix && strcmp(cfix[k].name, nm); k++) ;
    if (k == ncfix) { if (ncfix == 8) return; ncfix++; memcpy(cfix[k].name, nm, 18); }
    cfix[k].wide = wide; cfix[k].narrow = narrow;
}
static void cursor_fix(int narrowed) {
    uint32_t sc = R32(G_SELECT_CURSOR);
    if (!(sc >= 0x80000000u && sc < 0x94000000u)) return;
    for (int c = 0; c < 4; c++) {
        uint32_t e = sc + (uint32_t)c * SC_STRIDE;
        if (R8(e + SC_ON) != 1) continue;
        char nm[18]; for (int i = 0; i < 17; i++) nm[i] = (char)R8(e + SC_NAME + i); nm[17] = 0;
        for (int k = 0; k < ncfix; k++) if (!strcmp(cfix[k].name, nm)) {
            float w = rdf(e + SC_WIDTH), from = narrowed ? cfix[k].wide : cfix[k].narrow, to = narrowed ? cfix[k].narrow : cfix[k].wide;
            if (fabsf(w - from) < 0.01f && from != to) wrf(e + SC_WIDTH, to);
        }
    }
}
static const char *const card_panes[] = {"N_dateFile_00", "P_NumBase_01", "P_Shine_00", "W_dateFile_00", "W_shadow_00", "P_selectBase_00", "W_select_00"};

/* which of the setup scene's layouts this is */
enum { L_OTHER, L_FILE_SELECT, L_DATE_FILE, L_CHAR_SELECT };
static int which_layout(uint32_t layout, uint32_t root, int menu_kind, int *slot) {
    if (!in_setup() || !gs_scene) return L_OTHER;
    if (menu_kind == 2) return L_FILE_SELECT;
    uint32_t np = R32(gs_scene + GS_NUMPY);
    if (np && root == R32(np + NP_ROOT)) return L_CHAR_SELECT;
    for (int i = 0; i < 3; i++) {
        uint32_t df = R32(gs_scene + GS_DATEFILE + 4 * i);
        if (df && layout > df && layout - df < 0x1000) { *slot = i; return L_DATE_FILE; }
    }
    return L_OTHER;
}

/* before the layout's matrices are computed: positions */
void gs_layout_calc(uint32_t layout, uint32_t root, int menu_kind) {
    int slot = -1, w = which_layout(layout, root, menu_kind, &slot);
    if (w == L_FILE_SELECT) {
        /* singleplayer / Game Select: no versus buttons, so the cards and title sit lower
           (70), then everything 16 up */
        float dy = (menu_singleplayer() || phase == PH_GAME) ? -70.0f + 16.0f : 0.0f;
        pane_shift(lyt_find_pane(root, "N_fileSelect_00"), 1, dy);
        pane_shift(lyt_find_pane(root, "N_titlePos_00"), 1, dy);
    } else if (w == L_DATE_FILE) {
        /* Game Select: the card as wide as its game's picture (the picture is 155 high) */
        int cur = (int)R32(R32(gs_scene + GS_FSEL) + FS_CHOICE);
        gport_t *p = phase == PH_GAME && slot == cur ? game_portrait(cur_game()) : NULL;
        float d = p ? 155.0f * (1.0f - p->aspect) : 0.0f;
        if (p && d == 0) d = -0.0001f;                              /* (a square picture: still ours) */
        for (unsigned i = 0; i < sizeof card_panes / sizeof card_panes[0]; i++) pane_narrow(lyt_find_pane(root, card_panes[i]), d);
        if (phase == PH_GAME ? slot == cur : slot == 0) cursor_fix(p != NULL);   /* (once per frame) */
    } else if (w == L_CHAR_SELECT && !is_multi()) {
        /* singleplayer: Start Game alone, in the middle */
        uint32_t yes = lyt_find_pane(root, "P_yesBase_00");
        if (yes) pane_shift(yes, 0, -110.0f);
        /* "Back 1" stays while it shows (the button's animation squashes it to height 0;
           set before the matrices) */
        if (np_buttons) { uint32_t back = lyt_find_pane(root, "N_back");
            if (back) { lyt_set_visible(back, 1); lyt_set_alpha(back, 255); wrf(back + 0x44, 1.0f); wrf(back + 0x48, 1.0f); } }
    }
}

/* before the layout is drawn: what's shown, and the texts */
void gs_layout_draw(uint32_t layout, uint32_t root, int menu_kind) {
    int slot = -1, w = which_layout(layout, root, menu_kind, &slot);
    if (w == L_FILE_SELECT) {
        fs_drawn_frame = g_frames_presented;
        int game = phase == PH_GAME;
        uint32_t title = lyt_find_pane(root, "T_title_00");
        if (game) text_set(title, "Select a Game", 0); else text_restore(title);
        show_if(root, "P_copy_00", game, 0); show_if(root, "P_delete_00", game, 0);
        int no_vs = game || menu_singleplayer();
        show_if(root, "N_Multi_00", no_vs, 0); show_if(root, "N_MultiCoin_00", no_vs, 0);
        uint32_t back = lyt_find_pane(root, "N_back");                   /* "Back 1", as on the character select */
        if (back) { lyt_set_visible(back, 1); lyt_set_alpha(back, 255); }
    } else if (w == L_DATE_FILE) {
        int cur = (int)R32(R32(gs_scene + GS_FSEL) + FS_CHOICE), game = phase == PH_GAME;
        if (game && slot != cur) { vis_set(root, 0); return; }
        vis_restore(root);
        static const char *contents[] = {"N_dateFile_01", "P_fileNumBase_02", "N_save"};
        uint32_t t1 = lyt_find_pane(root, "T_NewdateFile_00"), t2 = lyt_find_pane(root, "T_NewdateFileS");
        for (int i = 0; i < 3; i++) show_if(root, contents[i], game, 0);
        gport_t *p = game ? game_portrait(cur_game()) : NULL;           /* the game's picture, or its name on a blank card */
        card_picture(lyt_find_pane(root, "P_NumBase_01"), p);
        show_if(root, "N_NewFile_00", game, !p);
        if (game && !p) { text_set_scaled(t1, GAMES[cur_game()].label, 0, 0.78f); text_set_scaled(t2, GAMES[cur_game()].label, 0, 0.78f); }
        else { text_restore(t1); text_restore(t2); }
    } else if (w == L_CHAR_SELECT) {
        text_set(lyt_find_pane(root, "T_titleNinzu_00"), is_multi() ? "Multiplayer" : "Singleplayer", 0);
        lyt_set_visible(lyt_find_pane(root, "N_remo_00"), 0);           /* (the four remotes) */
        text_set(lyt_find_pane(root, "T_guide_01"), "Settings ", 1);       /* was "Add Wii Remote +" */
        /* singleplayer: no Back button (1 takes the character back); "Back 1" stays */
        int single = !is_multi();
        show_if(root, "P_noBase_00", single, 0);
        show_if(root, "P_yesBase_00", single && !np_buttons, 0);         /* Start Game: only once the character is chosen */

    }
}
