/* rt_chars.c - the character list (docs/CHARACTERS.md).
 *
 * Characters are data here, separate from the 8 player slots: the roster below can grow past
 * 8. Each character has a *body* - one of the 8 player types the game knows (its 3D model,
 * voice, per-type lives and power-up) - and a *style* (normal, or a retro movement + sprite
 * look, rt_retro.c). Each player slot remembers which character it picked (g_player_char).
 *
 * Until the game's per-type data is widened past 8 types, characters share bodies:
 *  - a character with its own body (Mario ... Rosalina) is "taken" while another player who
 *    has decided uses that body;
 *  - Goomba and the retro characters borrow one: their preferred body if it is free, else the
 *    first free one (a retro character is drawn as a sprite, so the body only shows on the
 *    character select card and the HUD icon).
 *
 * The character select (dCharacterChangeSelectBase_c, NSMBW's cards) keeps its look; left/right
 * now walks the whole roster. mOption (0x2E0) still counts down from 4 like the original
 * (character index = 4 - mOption), so the range is 4 .. 4 - (CHAR_COUNT - 1). Hooks:
 *   mod_fn_2_3170  executeState_SelectWait   (native: the wider range)
 *   mod_fn_2_2600  calcContentsIcon          (native: the card gets 100 + 2 * character + locked)
 *   mod_fn_2_2510  isCharacterLocked
 *   mod_fn_2_26D0  initDecidedCharacter      (native: picks the body, remembers the character)
 *   mod_fn_2_26F0  initOption                (native: the cursor starts on the player's character)
 *   mod_fn_2_2770  readyContents             (+ the card picture in our encoding)
 *   mod_fn_2_E90, mod_fn_2_1290  the arrows  (their range ends -3 / 4 are mapped)
 *   mod_fn_2_5AB0  dCharacterChangeSelectContents_c::dispNextConPicture  (native: portraits)
 * Portraits: the layout has pictures for Mario, Luigi and the two Toads only; every other
 * character shows ui/characters/<id>/portrait.png (also overrides those four when present),
 * swapped into the card's Mario picture pane (its TexMap points at our texture). */
#include "rt.h"
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

/* ---- the roster ---- */
enum { MARIO, LUIGI, BLUE_TOAD, YELLOW_TOAD, TOADETTE, PURPLE_TOADETTE, ORANGE_TOAD, BLACK_TOAD, BODY_COUNT };
enum { STYLE_NORMAL, STYLE_SMB1, STYLE_SMW, STYLE_SM64 };
typedef struct {
    const char *id, *name;
    int body;            /* its own player type, or -1: borrows one */
    int prefer;          /* the body to borrow first (body < 0) */
    int style;
} chara_t;
static const chara_t ROSTER[] = {
    {"mario",       "Mario",              MARIO,           0, STYLE_NORMAL},
    {"luigi",       "Luigi",              LUIGI,           0, STYLE_NORMAL},
    {"ala_gold",    "Ala Gold",           YELLOW_TOAD,     0, STYLE_NORMAL},
    {"buckenberry", "Buckenberry",        BLUE_TOAD,       0, STYLE_NORMAL},
    {"toadette",    "Toadette",           TOADETTE,        0, STYLE_NORMAL},
    {"toadsworth",  "Toadsworth",         BLACK_TOAD,      0, STYLE_NORMAL},   /* model: Toad addon (rt_charmdl.c) */
    {"daisy",       "Daisy",              ORANGE_TOAD,     0, STYLE_NORMAL},   /* model: Rosalina and Daisy mod */
    {"rosalina",    "Rosalina",           PURPLE_TOADETTE, 0, STYLE_NORMAL},   /* model: Rosalina and Daisy mod */
    {"goomba",      "Goomba",             -1, BLACK_TOAD,      STYLE_NORMAL},  /* (needs a 9th body for its model) */
    {"retro_smb1",  "Retro Mario (SMB1)", -1, MARIO,           STYLE_SMB1},
    {"retro_smw",   "Retro Mario (SMW)",  -1, MARIO,           STYLE_SMW},     /* (style: to make) */
    {"retro_sm64",  "Retro Mario (SM64)", -1, MARIO,           STYLE_SM64},    /* (style: to make) */
};
#define CHAR_COUNT ((int)(sizeof ROSTER / sizeof ROSTER[0]))
#define TITLE_CHARS 5                 /* the first five are on the title screen */
#define OPT_HIGH 4                    /* mOption of character 0 */
#define OPT_LOW (OPT_HIGH - (CHAR_COUNT - 1))

static int g_player_char[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

/* ---- profiles (rt_profiles.c): per controller slot, the player's username and preferred
   character. Part of the save state: an online game has everyone's, the same on every PC. ---- */
static char slot_tag[8][5];
static int slot_pref[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
const char *chars_id(int c) { return c >= 0 && c < CHAR_COUNT ? ROSTER[c].id : ""; }
int chars_index(const char *id) { if (id) for (int i = 0; i < CHAR_COUNT; i++) if (!strcmp(ROSTER[i].id, id)) return i; return -1; }
void chars_set_slot_profile(int s, const char *name, int pref) {
    if (s < 0 || s >= 8) return;
    snprintf(slot_tag[s], sizeof slot_tag[s], "%s", name ? name : "");
    slot_pref[s] = pref >= 0 && pref < CHAR_COUNT ? pref : -1;
}
static int slot_of_player(int n) {                 /* the controller channel of the game's player n (-1) */
    uint32_t rm = R32(0x8042A028u); if (!rm || n < 0 || n >= 8) return -1;
    uint32_t con = R32(rm + 4 + 4 * n); return con ? (int)R32(con + 12) : -1;
}
const char *chars_tag_of_player(int n) { int s = slot_of_player(n); return s >= 0 && s < 8 ? slot_tag[s] : ""; }
const char *chars_tag_of_slot(int s) { return s >= 0 && s < 8 ? slot_tag[s] : ""; }

/* ---- guest addresses (USA rev 2) ---- */
#define MOD_PY_ENTRY      0x8185E890u   /* daPyMng_c::mPlayerEntry[8] */
#define MOD_PY_TYPE       0x8185E93Cu   /* daPyMng_c::mPlayerType[8] (by player) */
#define MOD_PY_REST       0x81851D8Cu   /* daPyMng_c::mRest[8] (by type) */
#define MOD_DEFAULT_TYPES 0x81849BE4u   /* dMj2dGame_c::scDefaultPlayerTypes[8] */
#define MOD_PY_MODE       0x8185E95Cu   /* daPyMng_c::mPlayerMode[8] (by type) */
#define MOD_PY_CREATE_ITEM 0x8185E97Cu  /* daPyMng_c::mCreateItem[8] (by type) */
#define G_GAME_FLAG       0x80429F80u   /* dInfo_c::mGameFlag */
#define GF_MULTI_MODE     0x10
#define MOD_KEY_REMOCON   0x8185FB1Cu   /* per channel: the controller object (vtable +0x20: trig(mask)) */
#define G_GAME_KEY        0x80429F50u   /* dGameKey_c::m_instance (+4: cores[8]) */
#define G_NOW_SCENE       0x80428732u   /* dScene_c::m_nowScene (u16) */
#define SCENE_WORLD_MAP   3
#define G_SND_SCENE_MGR   0x8042A4A8u
#define A_SET_SETUP_TRACK 0x8019BA20u   /* SndSceneMgr::setGameSetupTrack(track) */
#define A_UPDATE_REMOCON  0x8076FB20u   /* dCharacterChangeSelectBase_c::updateRemocon() */
#define A_LAYOUT_DISP_NUM 0x800B3B60u   /* dGameCom::LayoutDispNumber(const int&, const int&, LytTextBox_c*, bool) */
#define ST_CB_BUTTON_EXIT 0x80990AC0u   /* StateID_ButtonExitAnimeEndWait */
#define ST_CB_HIT_ANIME   0x80990C40u   /* StateID_HitAnimeEndWait */
#define ST_CB_MOVE_ANIME  0x80990C00u   /* StateID_MoveAnimeEndWait */
/* dCharacterChangeSelectBase_c */
#define CB_CONTENTS     0x074
#define CB_ARROW        0x078
#define CB_STATE        0x228
#define CB_CONNECT      0x280           /* PlyConnectStage_e* (by player): 3 = ENTER */
#define CB_ENTER_COUNT  0x288           /* int* */
#define CB_PLY_DECIDED  0x28C           /* PLAYER_TYPE_e* (by player) */
#define CB_MOVE_KIND    0x2A0           /* 4 right, 5 left, else: the option decided */
#define CB_DECIDED_CHAR 0x2D4           /* PLAYER_TYPE_e mDecidedCharacter */
#define CB_PLAYER_NO    0x2D8
#define CB_OPTION       0x2E0
#define CB_PLAYER_COUNT 0x2E8
#define CONNECT_ENTER   3
/* dCharacterChangeSelectArrow_c */
#define AR_MOVE_DIR     0x258
#define AR_PREV_OPTION  0x260
#define AR_OPTION       0x264
/* dCharacterChangeSelectContents_c */
#define CC_PICTURES     0x254           /* Picture*[8]: shadow 0/1, Luigi 2/3, blue Toad 4/5, yellow Toad 6/7 */
#define CC_TEXTBOXES    0x274           /* LytTextBox_c*[4]: lives 0/1, "x" 2/3 */
#define CC_ICON_SHOWN_NOW  0x28C        /* the icons dispNextConPicture last showed */
#define CC_ICON_SHOWN_NEXT 0x294
#define CC_ICON_NOW     0x290           /* side 0 */
#define CC_ICON_NEXT    0x298           /* side 1 */
#define CC_MARIO_PICS   0x2B8           /* Picture*[2] */
#define ICON_BASE       100             /* our encoding: 100 + 2 * character + locked */

/* our guest memory: scratch + portrait textures (below the renderer's texture limit) */
#define CHAR_MEM        0x81A00000u
#define CM_SCRATCH      (CHAR_MEM + 0x0000)
#define CM_TEX          (CHAR_MEM + 0x1000)
#define CM_TEX_END      (CHAR_MEM + 0x200000)

#define gcall rt_gcall
#define VFN(obj, off) R32(R32(obj) + (off))
static void change_state(ppc_context *ctx, uint8_t *base, uint32_t mgr, uint32_t state) { gcall(ctx, base, VFN(mgr, 0x18), 2, mgr, state); }
static void vis(uint32_t pane, int on) { if (pane) W8(pane + 0xBB, (R8(pane + 0xBB) & ~1) | (on ? 1 : 0)); }

/* ======================================================= the roster, for the rest of the runtime */
int chars_count(void) { return CHAR_COUNT; }
const char *chars_name(int c) { return c >= 0 && c < CHAR_COUNT ? ROSTER[c].name : "?"; }
/* the character of player slot n (-1: none known) - derived from its type when it never picked */
int chars_of_player(int n) {
    if (n < 0 || n >= 8) return -1;
    int type = (int)R32(MOD_PY_TYPE + 4 * n), c = g_player_char[n];
    if (c >= 0 && (ROSTER[c].body < 0 || ROSTER[c].body == type)) return c;
    for (int i = 0; i < CHAR_COUNT; i++) if (ROSTER[i].body == type) return i;
    return -1;
}
int chars_style_of_player(int n) { int c = chars_of_player(n); return c >= 0 ? ROSTER[c].style : STYLE_NORMAL; }
int chars_is_smb1(int n) { return chars_style_of_player(n) == STYLE_SMB1; }

/* the way player slot n last pressed left or right in the character select (-1 left, 1 right):
   the retro characters' figure there faces that way (rt_retro.c). Polled while the figure is
   drawn; SelectWait records the presses that walk the cards too. */
static int last_dir[8];
int chars_select_facing(int n) {
    if (n < 0 || n >= 8) return 1;
    uint32_t core = R32(G_GAME_KEY) ? R32(R32(G_GAME_KEY) + 4 + 4 * n) : 0;
    uint32_t trig = core ? R32(core + 28) : 0;
    if (trig & 8) last_dir[n] = -1; else if (trig & 4) last_dir[n] = 1;
    return last_dir[n] ? last_dir[n] : 1;
}

/* ======================================================= the title screen: the first five */
void chars_title_players(void) {
    for (int i = 0; i < 8; i++) {
        W32(MOD_PY_ENTRY + 4 * i, i < TITLE_CHARS);
        if (i < TITLE_CHARS) W32(MOD_PY_TYPE + 4 * i, (uint32_t)ROSTER[i].body);
    }
}

/* ======================================================= character select */
static int opt_char(int option) { int c = OPT_HIGH - option; return c < 0 ? 0 : c >= CHAR_COUNT ? CHAR_COUNT - 1 : c; }
static int player_entered(uint32_t cb, int ply) { return R32(R32(cb + CB_CONNECT) + 4 * ply) == CONNECT_ENTER; }
static int decided_body(uint32_t cb, int ply) { return (int)R32(R32(cb + CB_PLY_DECIDED) + 4 * ply); }

/* another player who has decided already has this character, or the body it needs */
static int char_locked(uint32_t cb, int c) {
    int me = (int)R32(cb + CB_PLAYER_NO), free_bodies = BODY_COUNT;
    for (int ply = 0; ply < 8; ply++) {
        if (ply == me || !player_entered(cb, ply)) continue;
        free_bodies--;
        if (g_player_char[ply] == c) return 1;
        if (ROSTER[c].body >= 0 && decided_body(cb, ply) == ROSTER[c].body) return 1;
    }
    return free_bodies <= 0;
}
/* the body for character c, for player `me` */
static int pick_body(uint32_t cb, int c) {
    if (ROSTER[c].body >= 0) return ROSTER[c].body;
    int me = (int)R32(cb + CB_PLAYER_NO), used[BODY_COUNT] = {0};
    for (int ply = 0; ply < 8; ply++) {
        if (ply == me || !player_entered(cb, ply)) continue;
        int b = decided_body(cb, ply);
        if (b >= 0 && b < BODY_COUNT) used[b] = 1;
    }
    if (!used[ROSTER[c].prefer]) return ROSTER[c].prefer;
    /* the bodies of characters that only have a placeholder model first, then the others */
    static const int order[BODY_COUNT] = {PURPLE_TOADETTE, ORANGE_TOAD, BLACK_TOAD, TOADETTE, BLUE_TOAD, YELLOW_TOAD, LUIGI, MARIO};
    for (int i = 0; i < BODY_COUNT; i++) if (!used[order[i]]) return order[i];
    return ROSTER[c].prefer;
}

static uint32_t card_icon(uint32_t cb, int option) {
    int c = opt_char(option);
    return (uint32_t)(ICON_BASE + 2 * c + (char_locked(cb, c) ? 1 : 0));
}

/* calcContentsIcon(swapIndex, baseIndex) */
void hle_mod_fn_2_2600(ppc_context *ctx, uint8_t *base) {
    uint32_t cb = ctx->r[3];
    W32(R32(cb + CB_CONTENTS) + (ctx->r[4] ? CC_ICON_NEXT : CC_ICON_NOW), card_icon(cb, (int)ctx->r[5]));
}
/* isCharacterLocked(type) - (only readyContents still asks by type) */
void hle_mod_fn_2_2510(ppc_context *ctx, uint8_t *base) {
    uint32_t cb = ctx->r[3]; int type = (int)ctx->r[4], c = 0;
    for (int i = 0; i < CHAR_COUNT; i++) if (ROSTER[i].body == type) { c = i; break; }
    ctx->r[3] = (uint32_t)char_locked(cb, c);
}
static int kept_choice[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
/* player slot n took its character back: its card comes back on that character */
void chars_keep_choice(int n) { if (n >= 0 && n < 8) kept_choice[n] = chars_of_player(n); }
static int char_locked(uint32_t cb, int c);
/* a player who hasn't picked yet this time: its profile's preferred character, if free (-1) */
static int profile_pick(uint32_t cb, int n) {
    if (n < 0 || n >= 8 || g_player_char[n] >= 0) return -1;
    int s = slot_of_player(n), pref = s >= 0 && s < 8 ? slot_pref[s] : -1;
    if (pref < 0 || char_locked(cb, pref)) return -1;
    static int logged[8]; if (logged[n] != pref + 1) { logged[n] = pref + 1; rt_log("chars", "player %d (%s): preferred %s", n + 1, slot_tag[s], ROSTER[pref].name); }
    return pref;
}
/* readyContents: the card shows the player's starting character */
void hle_mod_fn_2_2770(ppc_context *ctx, uint8_t *base) {
    uint32_t cb = ctx->r[3];
    int n = (int)R32(cb + CB_PLAYER_NO);
    orig_mod_fn_2_2770(ctx, base);                                      /* (sets the cursor to the slot's default) */
    if (n >= 0 && n < 8 && kept_choice[n] >= 0) { W32(cb + CB_OPTION, (uint32_t)(OPT_HIGH - kept_choice[n])); kept_choice[n] = -1; }
    else { int p = profile_pick(cb, n); if (p >= 0) W32(cb + CB_OPTION, (uint32_t)(OPT_HIGH - p)); }
    W32(R32(cb + CB_CONTENTS) + CC_ICON_NOW, card_icon(cb, (int)R32(cb + CB_OPTION)));
}
/* initOption: the cursor starts on the player's character - the first time, its profile's
   preferred character if nobody else has it */
void hle_mod_fn_2_26F0(ppc_context *ctx, uint8_t *base) {
    uint32_t cb = ctx->r[3];
    int n = (int)R32(cb + CB_PLAYER_NO), c = chars_of_player(n);
    { int p = profile_pick(cb, n); if (p >= 0) c = p; }
    W32(cb + CB_OPTION, (uint32_t)(OPT_HIGH - (c < 0 ? 0 : c)));
}
/* ---- singleplayer: lives and power-up belong to the save file, not to the character ----
   The game keeps them per player type (body). In singleplayer the save file's state lives in
   Mario's slot (type 0 - the original game only ever played Mario there): when player 1 picks a
   character its body gets that state, and before saving it goes back to slot 0. (Multiplayer:
   per character, i.e. per body, as the game does.) */
static int singleplayer(void) { return !(R32(G_GAME_FLAG) & GF_MULTI_MODE); }
static void copy_type_state(int from, int to) {
    if (from == to || from < 0 || from >= BODY_COUNT || to < 0 || to >= BODY_COUNT) return;
    W32(MOD_PY_REST + 4 * to, R32(MOD_PY_REST + 4 * from));
    W32(MOD_PY_MODE + 4 * to, R32(MOD_PY_MODE + 4 * from));
    W32(MOD_PY_CREATE_ITEM + 4 * to, R32(MOD_PY_CREATE_ITEM + 4 * from));
}
/* dSaveMng_c::prepareSave */
void hle_mod_fn_800E1690(ppc_context *ctx, uint8_t *base) {
    if (singleplayer()) copy_type_state((int)R32(MOD_PY_TYPE), MARIO);
    orig_mod_fn_800E1690(ctx, base);
}

/* initDecidedCharacter: the body for the chosen character */
void hle_mod_fn_2_26D0(ppc_context *ctx, uint8_t *base) {
    uint32_t cb = ctx->r[3];
    int c = opt_char((int)R32(cb + CB_OPTION)), me = (int)R32(cb + CB_PLAYER_NO);
    int body = pick_body(cb, c);
    if (singleplayer() && me == 0) copy_type_state((int)R32(MOD_PY_TYPE), body);   /* state carries over */
    W32(cb + CB_DECIDED_CHAR, (uint32_t)body);
    if (me >= 0 && me < 8) g_player_char[me] = c;
    rt_log("chars", "player %d: %s (body %d)", me + 1, ROSTER[c].name, body);
}

/* testing: RT_SELECT_SCRIPT="R:6 :20 L:6 ..." buttons from the first frame the character select
   waits for a choice (R/L right/left, A = 2, 1 = 1; n frames each) */
static void select_script_step(void) {
    extern uint32_t g_retro_test_buttons;
    static int init, left; static char buf[1024], *save, *tok; static const char *spec;
    if (!init) { init = 1; spec = getenv("RT_SELECT_SCRIPT"); if (spec) { snprintf(buf, sizeof buf, "%s", spec); tok = strtok_r(buf, " ", &save); left = -1; } }
    if (!spec) return;
    while (tok && left == 0) { tok = strtok_r(NULL, " ", &save); left = -1; }
    if (!tok) { g_retro_test_buttons = 0; return; }
    if (left < 0) { const char *c = strchr(tok, ':'); left = c ? atoi(c + 1) : 1; }
    uint32_t b = 0;
    for (const char *k = tok; *k && *k != ':'; k++) b |= *k == 'R' ? 0x0004 : *k == 'L' ? 0x0008 : *k == 'A' ? 0x0100 : *k == '1' ? 0x0200 : 0;
    if (b && left > 0) rt_log("chars", "test: buttons %X", b);
    g_retro_test_buttons = b; left--;
}
static uint64_t select_seen_frame;
void chars_test_frame(void) { extern uint64_t g_frames_presented; if (select_seen_frame && g_frames_presented != select_seen_frame) { select_seen_frame = g_frames_presented; select_script_step(); } }

/* executeState_SelectWait: the module's version with the roster's range */
void hle_mod_fn_2_3170(ppc_context *ctx, uint8_t *base) {
    uint32_t cb = ctx->r[3];
    { extern uint64_t g_frames_presented; if (!select_seen_frame) { select_seen_frame = g_frames_presented + 1; rt_log("chars", "select waits (frame %llu)", (unsigned long long)g_frames_presented); } }
    if (gcall(ctx, base, A_UPDATE_REMOCON, 1, cb)) {
        W32(cb + CB_PLAYER_NO, (uint32_t)-1);
        change_state(ctx, base, cb + CB_STATE, ST_CB_BUTTON_EXIT);
        return;
    }
    int option = (int)R32(cb + CB_OPTION);
    W32(R32(cb + CB_CONTENTS) + CC_ICON_NOW, card_icon(cb, option));
    uint32_t core = R32(R32(G_GAME_KEY) + 4 + 4 * R32(cb + CB_PLAYER_NO));
    uint32_t rc = R32(MOD_KEY_REMOCON + 4 * R32(core + 4));
    if (gcall(ctx, base, VFN(rc, 0x20), 2, rc, 0x900)) {                 /* confirm (2 / A) */
        if (char_locked(cb, opt_char(option))) return;
        uint32_t ec = R32(cb + CB_ENTER_COUNT);
        int n = (int)R32(ec) + 1, count = (int)R32(cb + CB_PLAYER_COUNT);
        W32(ec, (uint32_t)n);
        if (n >= count) {
            W32(ec, (uint32_t)count);
            if (R16(G_NOW_SCENE) != SCENE_WORLD_MAP) gcall(ctx, base, A_SET_SETUP_TRACK, 2, R32(G_SND_SCENE_MGR), 3);
        }
        W32(cb + CB_MOVE_KIND, (uint32_t)option);
        change_state(ctx, base, cb + CB_STATE, ST_CB_HIT_ANIME);
        return;
    }
    uint32_t trig = R32(core + 28);
    {   int me = (int)R32(cb + CB_PLAYER_NO);
        if (me >= 0 && me < 8 && (trig & 12)) last_dir[me] = trig & 8 ? -1 : 1; }
    if (trig & 8) {                                                      /* left: the previous character */
        if (option >= OPT_HIGH) return;
        W32(R32(cb + CB_ARROW) + AR_MOVE_DIR, 1);
        W32(cb + CB_MOVE_KIND, 5); option++;
    } else if (trig & 4) {                                               /* right: the next one */
        if (option <= OPT_LOW) return;
        W32(R32(cb + CB_ARROW) + AR_MOVE_DIR, 2);
        W32(cb + CB_MOVE_KIND, 4); option--;
    } else return;
    W32(cb + CB_OPTION, (uint32_t)option);
    change_state(ctx, base, cb + CB_STATE, ST_CB_MOVE_ANIME);
}

/* the arrows hide at the ends of the list: the module's code knows the ends as -3 and 4 */
static int arrow_map(int o) { return o <= OPT_LOW ? -3 : o >= OPT_HIGH ? 4 : o < -2 ? -2 : o; }
static void arrow_call(ppc_context *ctx, uint8_t *base, void (*fn)(ppc_context *, uint8_t *)) {
    uint32_t ar = ctx->r[3];
    int now = (int)R32(ar + AR_OPTION), prev = (int)R32(ar + AR_PREV_OPTION);
    W32(ar + AR_OPTION, (uint32_t)arrow_map(now));
    W32(ar + AR_PREV_OPTION, (uint32_t)(prev == now ? arrow_map(now) : arrow_map(prev) == arrow_map(now) ? 99 : arrow_map(prev)));
    fn(ctx, base);
    int prev_after = (int)R32(ar + AR_PREV_OPTION);
    W32(ar + AR_PREV_OPTION, (uint32_t)(prev_after == arrow_map(now) ? now : prev));
    W32(ar + AR_OPTION, (uint32_t)now);
}
void hle_mod_fn_2_E90(ppc_context *ctx, uint8_t *base) { arrow_call(ctx, base, orig_mod_fn_2_E90); }
void hle_mod_fn_2_1290(ppc_context *ctx, uint8_t *base) { arrow_call(ctx, base, orig_mod_fn_2_1290); }

/* ======================================================= portraits */
typedef struct { int state; uint32_t tex; int w, h; } portrait_t;     /* state 0 untried, 1 ok, -1 none */
static portrait_t portraits[32];
static uint32_t tex_next = CM_TEX;
static portrait_t *portrait(int c) {
    portrait_t *p = &portraits[c];
    if (p->state) return p->state > 0 ? p : NULL;
    p->state = -1;
    char path[1200]; snprintf(path, sizeof path, "%s/ui/characters/%s/portrait.png", g_root, ROSTER[c].id);
    int w, h; uint32_t *px = ui_load_png(path, &w, &h);
    if (!px) return NULL;
    int tw = (w + 3) & ~3, th = (h + 3) & ~3;
    uint32_t bytes = (uint32_t)tw * th * 4;
    if (w > 1024 || h > 1024 || tex_next + bytes > CM_TEX_END) { free(px); rt_log("chars", "%s: too big", path); return NULL; }
    uint8_t *dst = G(tex_next);                     /* GX RGBA8 (4x4 tiles: AR pairs, then GB pairs) */
    for (int ty = 0; ty < th; ty += 4) for (int tx = 0; tx < tw; tx += 4) {
        for (int i = 0; i < 16; i++) {
            int x = tx + (i & 3), y = ty + (i >> 2);
            uint32_t c32 = (x < w && y < h) ? px[y * w + x] : 0;
            dst[i * 2] = (uint8_t)(c32 >> 24); dst[i * 2 + 1] = (uint8_t)(c32 >> 16);
            dst[32 + i * 2] = (uint8_t)(c32 >> 8); dst[32 + i * 2 + 1] = (uint8_t)c32;
        }
        dst += 64;
    }
    free(px);
    p->tex = tex_next; p->w = w; p->h = h; p->state = 1;   /* (GX pads the tiles itself) */
    tex_next = (tex_next + bytes + 31) & ~31u;
    rt_log("chars", "portrait %s: %dx%d", ROSTER[c].id, w, h);
    return p;
}

/* nw4r::lyt::TexMap of a Picture pane's material: { image, palette, u16 w, h, f32 minLOD, maxLOD,
   u16 bias, palEntries, u32 bits (format in the top 4 bits) } - first in the material's GX memory */
#define PANE_MATERIAL 0x28
#define MAT_GXMEM     0x40          /* (seen: the TexMaps follow the material, at +0x6C) */
static uint32_t pane_texmap(uint32_t pane) {
    uint32_t mat = pane ? R32(pane + PANE_MATERIAL) : 0;
    uint32_t gx = mat ? R32(mat + MAT_GXMEM) : 0;
    return gx >= 0x80000000u && gx < 0x94000000u ? gx : 0;
}
/* the pane's own texture is kept to put back; refreshed whenever the TexMap doesn't point at one
   of ours (a new layout may reuse a pane address) */
typedef struct { uint32_t pane, image, wh, bits; } saved_tex_t;
static saved_tex_t saved[32];
static int ours(uint32_t image) { return image >= CM_TEX && image < CM_TEX_END; }
static void pane_texture(uint32_t pane, portrait_t *p) {
    uint32_t tm = pane_texmap(pane);
    if (!tm) return;
    saved_tex_t *s = NULL;
    for (int i = 0; i < 32 && !s; i++) if (saved[i].pane == pane) s = &saved[i];
    for (int i = 0; i < 32 && !s; i++) if (!saved[i].pane) { s = &saved[i]; s->pane = pane; }
    if (!s) { s = &saved[0]; s->pane = pane; }
    if (!ours(R32(tm))) { s->image = R32(tm); s->wh = R32(tm + 8); s->bits = R32(tm + 0x18); }
    if (p) {
        W32(tm, p->tex); W16(tm + 8, (uint16_t)p->w); W16(tm + 10, (uint16_t)p->h);
        W32(tm + 0x18, (s->bits & 0x0FFFFFFFu) | (6u << 28));            /* RGBA8 */
    } else if (ours(R32(tm))) {
        W32(tm, s->image); W32(tm + 8, s->wh); W32(tm + 0x18, s->bits);
    }
}
/* the layout's own portrait pane for the first four characters (Contents' picture index) */
static int layout_picture(int c) {
    switch (c) { case 0: return -1; case 1: return 2; case 2: return 6; case 3: return 4; default: return 0; }
}

/* dCharacterChangeSelectContents_c::dispNextConPicture(icon, side): the card's two faces
   (N_cSelectCon_00 / _01) slide past each other when the character changes; side 0 is the
   card's character, side 1 the one coming in. Each shows its character's picture; the base
   layout's N_scissorArea_00 clips them to the card (m2d clip - see rt_aspect.c GXSetScissor). */
void hle_mod_fn_2_5AB0(ppc_context *ctx, uint8_t *base) {
    uint32_t cc = ctx->r[3], icon = ctx->r[4], side = ctx->r[5];
    if (icon < ICON_BASE || icon >= ICON_BASE + 2u * CHAR_COUNT || side > 1) { orig_mod_fn_2_5AB0(ctx, base); return; }
    int c = (int)(icon - ICON_BASE) / 2, locked = (icon - ICON_BASE) & 1;
    for (uint32_t i = side; i < 8; i += 2) vis(R32(cc + CC_PICTURES + 4 * i), 0);
    vis(R32(cc + CC_MARIO_PICS + 4 * side), 0);
    for (uint32_t i = side; i < 4; i += 2) vis(R32(cc + CC_TEXTBOXES + 4 * i), 0);
    /* lives of the character's body */
    int body = ROSTER[c].body >= 0 ? ROSTER[c].body : ROSTER[c].prefer;
    W32(CM_SCRATCH, R32(MOD_PY_REST + 4 * body)); W32(CM_SCRATCH + 4, 2);
    gcall(ctx, base, A_LAYOUT_DISP_NUM, 4, CM_SCRATCH, CM_SCRATCH + 4, R32(cc + CC_TEXTBOXES + 4 * side), 0);
    /* picture */
    uint32_t mario = R32(cc + CC_MARIO_PICS + 4 * side);
    portrait_t *p = portrait(c);
    int lp = layout_picture(c);
    if (p) { pane_texture(mario, p); vis(mario, 1); }
    else if (lp < 0) { pane_texture(mario, NULL); vis(mario, 1); }
    else if (lp > 0) vis(R32(cc + CC_PICTURES + 4 * (lp + side)), 1);
    if (locked) vis(R32(cc + CC_PICTURES + 4 * side), 1);
    vis(R32(cc + CC_TEXTBOXES + 4 * side), 1);
    vis(R32(cc + CC_TEXTBOXES + 4 * (2 + side)), 1);
    {   static int dump = -1; if (dump < 0) dump = getenv("RT_CHARDUMP") != NULL;
        if (dump) {
            uint32_t tm = pane_texmap(mario);
            extern uint64_t g_frames_presented;
            rt_log("chars", "t=%.1f card side %u: %s%s, mario pane %08X material %08X texmap %08X: image %08X %ux%u bits %08X",
                   g_frames_presented / 60.0, side, ROSTER[c].name, locked ? " (taken)" : "", mario, mario ? R32(mario + PANE_MATERIAL) : 0, tm,
                   tm ? R32(tm) : 0, tm ? R16(tm + 8) : 0, tm ? R16(tm + 10) : 0, tm ? R32(tm + 0x18) : 0);
        }
    }
}

