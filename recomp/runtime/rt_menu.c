/* rt_menu.c - menus added to the game's title screen.
 *
 * Title screen: "Press 2 to Start" works as usual until 2 is pressed; then "Press 2 to Start" and
 * the copyright line fade out and a list fades in: Singleplayer / Multiplayer / Options, drawn
 * with the game's own font (taken from the "Press 2" text box). While the list is up the title
 * doesn't time out into its demo movie.
 *   Singleplayer - Game Select -> File Select -> character select (1 player) -> game
 *   Multiplayer  - the same with the 8-player character select
 *                  (those screens: rt_gamesetup.c)
 *   Options      - display, window, resolution, aspect ratio, HD textures, real Wii Remote;
 *                  saved to settings.ini; "Restart" appears when a change needs one
 * Input: the remote held sideways (d-pad moves, 2/A chooses, 1/B goes back). While our menus are
 * up the game sees no buttons. Drawn as an overlay (menu_draw) on a 640x480 virtual screen. */
#include "rt.h"
#include <math.h>
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */
extern uint64_t g_frames_presented;

/* guest state: current scene (dScene_c::m_nowScene) and stage game mode (dScStage_c::m_gameMode) */
#define G_NOW_SCENE 0x80428732u
#define G_GAME_MODE 0x8042A204u
enum { SCENE_WORLD_MAP = 3, SCENE_STAGE = 5, SCENE_GAME_SETUP = 10, MODE_TITLE = 2 };

/* Wii Remote core buttons; held sideways the d-pad's right is "up" on screen */
enum { B_LEFT = 0x0001, B_RIGHT = 0x0002, B_DOWN = 0x0004, B_UP = 0x0008, B_PLUS = 0x0010, B_2 = 0x0100, B_1 = 0x0200,
       B_B = 0x0400, B_A = 0x0800, B_MINUS = 0x1000, B_HOME = 0x8000 };
#define NAV_UP B_RIGHT
#define NAV_DOWN B_LEFT
#define NAV_LEFT B_UP
#define NAV_RIGHT B_DOWN

static int at_title(void) { return R16(G_NOW_SCENE) == SCENE_STAGE && R32(G_GAME_MODE) == MODE_TITLE; }
static double now_s(void) { return rt_emu_ns() / 1e9; }
static double now_wall(void) { return rt_now_ns() / 1e9; }

/* ================================================================== the game's font
 * nw4r::ut::ResFont (pointer at TextBox+0xE4): +0x14 = FINF (font info, pointers relocated). */
static struct {
    int ok, tex;
    int cell_w, cell_h, row, line, sheet_w, sheet_h, nsheets, height;
    int def_left, def_glyph, def_char;
    uint32_t widths, cmap;
    float atlas_w, atlas_h; int per_row;
} gf RT_LOCAL;                                    /* (its texture is the renderer's) */
static int valid_ptr(uint32_t p) { return (p >= 0x80000000u && p < 0x81900000u) || (p >= 0x90000000u && p < 0x94000000u); }
static void font_load_obj(uint32_t font);
static void font_load(uint32_t textbox) {
    static int tries RT_LOCAL; if (tries++ > 3) return;
    font_load_obj(R32(textbox + 0xE4));
}
/* the boot (update screen): the same font, before the title's text box exists */
#define G_FONT_PRESS 0x80358848u                          /* (the font object "Press 2 to Start" uses) */
static void font_load_boot(void) {
    static int tries RT_LOCAL;
    if (gf.ok || tries > 3) return;
    uint32_t finf = R32(G_FONT_PRESS + 0x14);
    if (!valid_ptr(finf) || !valid_ptr(R32(finf + 8))) return;
    tries++; font_load_obj(G_FONT_PRESS);
}
static void font_load_obj(uint32_t font) {
    uint32_t finf = valid_ptr(font) ? R32(font + 0x14) : 0, tglp = valid_ptr(finf) ? R32(finf + 8) : 0;
    rt_log("menu", "font object %08X FINF %08X TGLP %08X", font, finf, tglp);
    if (!valid_ptr(tglp)) return;
    gf.def_left = (int8_t)R8(finf + 4); gf.def_glyph = R8(finf + 5); gf.def_char = (int8_t)R8(finf + 6);
    gf.widths = R32(finf + 0xC); gf.cmap = R32(finf + 0x10); gf.height = R8(finf + 0x14);
    gf.cell_w = R8(tglp); gf.cell_h = R8(tglp + 1);
    uint32_t sheet_size = R32(tglp + 4);
    gf.nsheets = R16(tglp + 8); int fmt = R16(tglp + 0xA) & 0x7FFF;
    gf.row = R16(tglp + 0xC); gf.line = R16(tglp + 0xE); gf.sheet_w = R16(tglp + 0x10); gf.sheet_h = R16(tglp + 0x12);
    uint32_t img = R32(tglp + 0x14);
    if (!gf.height) gf.height = gf.cell_h;
    if (!valid_ptr(img) || gf.nsheets < 1 || gf.nsheets > 64 || gf.sheet_w > 1024 || gf.sheet_h > 1024 || !gf.row || !gf.line) {
        rt_log("menu", "font: unexpected glyph info (img %08X sheets %d %dx%d row %d line %d)", img, gf.nsheets, gf.sheet_w, gf.sheet_h, gf.row, gf.line); return; }
    /* sheets side by side (wrapping at 8192 pixels) in one atlas */
    gf.per_row = 8192 / gf.sheet_w; if (gf.per_row < 1) gf.per_row = 1; if (gf.per_row > gf.nsheets) gf.per_row = gf.nsheets;
    int rows = (gf.nsheets + gf.per_row - 1) / gf.per_row, aw = gf.per_row * gf.sheet_w, ah = rows * gf.sheet_h;
    uint32_t *atlas = calloc((size_t)aw * ah, 4);
    for (int i = 0; i < gf.nsheets; i++) {
        uint32_t *px = gx_decode_rgba(img + (uint32_t)i * sheet_size, gf.sheet_w, gf.sheet_h, fmt);
        if (!px) continue;
        int ox = (i % gf.per_row) * gf.sheet_w, oy = (i / gf.per_row) * gf.sheet_h;
        for (int y = 0; y < gf.sheet_h; y++) memcpy(atlas + (size_t)(oy + y) * aw + ox, px + (size_t)y * gf.sheet_w, (size_t)gf.sheet_w * 4);
        free(px);
    }
    gf.tex = ui_register_texture(atlas, aw, ah);
    free(atlas);
    gf.atlas_w = (float)aw; gf.atlas_h = (float)ah;
    gf.ok = gf.tex != 0;
    rt_log("menu", "game font: %d sheets %dx%d (format %d), cells %dx%d, height %d", gf.nsheets, gf.sheet_w, gf.sheet_h, fmt, gf.cell_w, gf.cell_h, gf.height);
}
static int glyph_index(uint32_t code) {
    uint32_t m = gf.cmap;
    for (int guard = 0; valid_ptr(m) && guard < 256; guard++) {
        uint32_t b = R16(m), e = R16(m + 2), method = R16(m + 4), next = R32(m + 8), info = m + 0xC;
        if (code >= b && code <= e) {
            if (method == 0) return R16(info) + (int)(code - b);
            if (method == 1) { int i = R16(info + 2 * (code - b)); return i == 0xFFFF ? -1 : i; }
            int n = R16(info);
            for (int k = 0; k < n; k++) if (R16(info + 2 + 4 * k) == code) return R16(info + 4 + 4 * k);
            return -1;
        }
        m = next;
    }
    return -1;
}
static void glyph_width(int idx, int *left, int *glyph, int *adv) {
    uint32_t w = gf.widths;
    for (int guard = 0; valid_ptr(w) && guard < 256; guard++) {
        int b = R16(w), e = R16(w + 2);
        if (idx >= b && idx <= e) { uint32_t t = w + 8 + 3 * (uint32_t)(idx - b); *left = (int8_t)R8(t); *glyph = R8(t + 1); *adv = (int8_t)R8(t + 2); return; }
        w = R32(w + 4);
    }
    *left = gf.def_left; *glyph = gf.def_glyph; *adv = gf.def_char;
}
/* width of a string in virtual pixels at this height */
static float gtext_width(const char *s, float size) {
    if (!gf.ok) return (float)strlen(s) * size;
    float sc = size / gf.height, w = 0;
    for (; *s; s++) { int idx = glyph_index((uint8_t)*s), l, g, a; if (idx < 0) idx = glyph_index('?'); glyph_width(idx < 0 ? 0 : idx, &l, &g, &a); w += a * sc; }
    return w;
}
/* draw a string with the game's font; size = line height in virtual pixels */
static void gtext(float x, float y, float size, const char *s, uint32_t rgba) {
    if (!gf.ok) { ui_text(x, y + size * 0.15f, size / 10.0f, s, rgba); return; }
    float sc = size / gf.height;
    int per = gf.row * gf.line;
    for (; *s; s++) {
        int idx = glyph_index((uint8_t)*s);
        if (idx < 0) idx = glyph_index('?');
        if (idx < 0) continue;
        int l, g, a; glyph_width(idx, &l, &g, &a);
        int sheet = idx / per, cell = idx % per;
        float cx = (float)((cell % gf.row) * (gf.cell_w + 1) + (sheet % gf.per_row) * gf.sheet_w);
        float cy = (float)((cell / gf.row) * (gf.cell_h + 1) + (sheet / gf.per_row) * gf.sheet_h);
        if (*s != ' ')
            ui_image(gf.tex, x + l * sc, y, g * sc, gf.cell_h * sc,
                     cx / gf.atlas_w, cy / gf.atlas_h, (cx + g) / gf.atlas_w, (cy + gf.cell_h) / gf.atlas_h, rgba);
        x += a * sc;
    }
}
static void gtext_centered(float cx, float y, float size, const char *s, uint32_t rgba) { gtext(cx - gtext_width(s, size) / 2, y, size, s, rgba); }

/* ================================================================== title screen state */
enum { T_IDLE, T_PRESS_OUT, T_LIST, T_LIST_OUT, T_PRESS_IN, T_OPTIONS, T_START, T_CONTROLS, T_ONLINE, T_PROFILES };
enum { NEXT_NONE, NEXT_SINGLE, NEXT_MULTI, NEXT_OPTIONS, NEXT_TITLE, NEXT_ONLINE };
#define FADE 0.25
static int tstate = T_IDLE, next_action, sel, opt_sel;
static double t_state;
static uint32_t prev_btn;
static uint64_t inject_until, inject_t0;           /* replaying a 2 press to the game */
static int play_mode;                              /* 1 Singleplayer, 2 Multiplayer (chosen on the title) */
int menu_singleplayer(void) { return play_mode != 2; }
/* title layout */
static uint32_t press_pane, copy_pane; static uint64_t title_seen_frame; static int press_ready;
static float press_x = 320, press_y = 380, press_size = 35;
/* file select layout */
/* the game setup screens (rt_gamesetup.c) */
uint32_t gs_filter_buttons(uint32_t b);
int gs_busy(void);
void gs_layout_calc(uint32_t layout, uint32_t root, int menu_kind);
void gs_layout_draw(uint32_t layout, uint32_t root, int menu_kind);
/* the Controls screen opened from the character select ("Settings +") */
static int ctl_open; static double ctl_t; static uint32_t ctl_hold;   /* buttons held when it closed */

static void set_state(int s) { tstate = s; t_state = now_s(); }
static float fade_t(void) { double t = (now_s() - t_state) / FADE; return t < 0 ? 0 : t > 1 ? 1 : (float)t; }
static int title_live(void) { return at_title() && g_frames_presented - title_seen_frame <= 3; }

/* ---- layout hook: runs before each game layout draws ---- */
static uint32_t roots[32], root_kind[32]; static int nroots;   /* 1 title, 2 file select, 3 other */
static int layout_kind(uint32_t root) {
    int kind = 0, k;
    uint32_t key = root ^ (R32(root + 0x14) * 2654435761u);    /* layout roots get reused: include the first child */
    for (k = 0; k < nroots; k++) if (roots[k] == key) return (int)root_kind[k];
    if (lyt_find_pane(root, "T_Press_00") && lyt_find_pane(root, "N_Title_00")) kind = 1;
    else if (lyt_find_pane(root, "N_MultiCoin_00") && lyt_find_pane(root, "N_fileSelect_00")) kind = 2;
    else { uint32_t c = R32(root + 0x14); char n[18] = ""; if (c && c != root + 0x14) for (int i = 0; i < 17; i++) n[i] = (char)R8(c - 4 + 0xBC + i);
           kind = !strcmp(n, "N_onoff_00") ? 4 : 3; }               /* 4: "Select Players" */
    if (nroots == 32) nroots = 0;
    roots[nroots] = key; root_kind[nroots++] = (uint32_t)kind;
    return kind;
}
static float rdf(uint32_t a) { uint32_t u = R32(a); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); W32(a, u); }
void boot_layout_calc(uint32_t layout, uint32_t root);        /* rt_bootscreen.c */
void boot_layout_draw(uint32_t layout, uint32_t root);
void menu_layout_calc_hook(uint32_t layout, uint32_t root) {
    boot_layout_calc(layout, root);
    gs_layout_calc(layout, root, layout_kind(root));
}
/* ---- title demo: players and camera ---- */
static uint32_t title_players[8];                  /* dAcPy_c actors, from daPyMng_c::setPlayer */
void hle_setPlayer__9daPyMng_cFiP7dAcPy_c(ppc_context *ctx, uint8_t *base) {
    if (ctx->r[3] < 8) title_players[ctx->r[3]] = ctx->r[4];
    orig_setPlayer__9daPyMng_cFiP7dAcPy_c(ctx, base);
}
static void title_trace(void) {
    static int on = -1; if (on < 0) on = getenv("RT_TITLETRACE") != NULL;
    if (!on || g_frames_presented % 20) return;
    uint32_t bp = R32(0x80429E00);   /* dBgParameter_c::ms_Instance_p (r13-22656) */
    char buf[1200]; int n = snprintf(buf, sizeof buf, "f %llu stage %u cam %.1f,%.1f |", (unsigned long long)g_frames_presented, R32(0x8042A200), bp ? rdf(bp + 0x38) : 0, bp ? rdf(bp + 0x3C) : 0);
    for (int i = 0; i < 8; i++) if (title_players[i]) n += snprintf(buf + n, sizeof buf - n, " p%d %.1f,%.1f sp %.2f mx %.2f", i, rdf(title_players[i] + 0xAC), rdf(title_players[i] + 0xB0), rdf(title_players[i] + 0xE8), rdf(title_players[i] + 0xF4));
    if (title_players[0] && g_frames_presented % 200 == 0) for (int o = 0; o < 0x2400; o += 4) { float v = rdf(title_players[0] + o); if (v == 1.5f && n < 1100) n += snprintf(buf + n, sizeof buf - n, " %X", o); }
    { uint32_t bg = R32(0x80429DD0u); if (bg) n += snprintf(buf + n, sizeof buf - n, " loop %.1f..%.1f w %.1f type %u", rdf(bg + 0x8FE64), rdf(bg + 0x8FE68), rdf(bg + 0x8FEA0), R32(0x8042A208u)); }
    rt_log("tdemo", "%s", buf);
}
/* Title screen: normally the camera follows the walking players (dBg_c camera execute,
   fn_80154F60). While our list is open the camera keeps going on its own at walking speed
   (wrapping with the stage's loop), and the players speed up to a run, outrun it and are kept
   just past the right edge. On Back they start just past the left edge, run in, and each drops
   back to a walk at its old place on screen; then the camera follows them again. The title
   timeout doesn't count the time the list was open. */
#define G_STAGE_COUNT 0x8042A200u
#define G_BG_PARAM 0x80429E00u                     /* dBgParameter_c::ms_Instance_p: +0x38 screen left, +0x40 width */
#define G_BG 0x80429DD0u                           /* dBg_c::m_bg_p: loop +0x8FE64 left, +0x8FE68 right, +0x8FEA0 width */
#define P_SPEEDF 0x10C                             /* player forward speed / its target */
#define P_MAXSPEEDF 0x110
#define WALK 1.5f
#define RUN 3.25f
enum { CAM_FOLLOW, CAM_OUTRUN, CAM_PARKED, CAM_RUNIN };
static int cam_mode; static float cam_x, cam_rel[8]; static uint32_t cam_freeze_count, cam_paused, cam_last_count;
static int cam_held(void) { return cam_mode != CAM_FOLLOW && R32(0x8042A204u) == 2; }   /* title (game mode 2) */
static void cam_trace(void) {
    static int on = -1; if (on < 0) on = getenv("RT_CAMTRACE") != NULL;
    if (!on || g_frames_presented % 60) return;
    static uint64_t last; if (last == g_frames_presented) return; last = g_frames_presented;
    uint32_t bp = R32(G_BG_PARAM), bg = R32(0x80429DD0u);
    char b[1400]; int n = 0;
    if (bp) for (int o = 0x34; o < 0x80; o += 4) n += snprintf(b + n, sizeof b - n, " bp%X=%.1f", o, rdf(bp + o));
    if (bg) for (int o = 0; o < 0x100; o += 4) { float v = rdf(bg + 0x90000 + o); if (v != 0 && v > -100000 && v < 100000 && (v - (int)v != 0 || (v > 1 && v < 20000))) n += snprintf(b + n, sizeof b - n, " g%X=%.2f", o, v); }
    rt_log("cam", "f%llu%s", (unsigned long long)g_frames_presented, b);
}
static float screen_w(void) { uint32_t bp = R32(G_BG_PARAM); float w = bp ? rdf(bp + 0x40) : 0; return w > 150 && w < 600 ? w : 300; }
/* the camera, driven by us while held: walking speed, wrapped with the stage loop */
void hle_fn_80154F60(ppc_context *ctx, uint8_t *base) {
    cam_trace();
    extern void aspect_camera_fix(void);
    orig_fn_80154F60(ctx, base);
    if (!cam_held()) { aspect_camera_fix(); return; }
    uint32_t bp = R32(G_BG_PARAM), bg = R32(G_BG);
    if (!bp || !bg) return;
    cam_x += WALK;
    float lw = rdf(bg + 0x8FEA0), lr = rdf(bg + 0x8FE68), w = screen_w();
    if (lw > 100 && cam_x + w * 0.5f > lr) {               /* past the loop's end: same view one loop back */
        cam_x -= lw;
        for (int i = 0; i < 8; i++) if (title_players[i] && rdf(title_players[i] + 0xAC) > lr - lw * 0.5f) {   /* (not already wrapped by the game) */
            wrf(title_players[i] + 0xAC, rdf(title_players[i] + 0xAC) - lw); wrf(title_players[i] + 0xB8, rdf(title_players[i] + 0xB8) - lw); }
    }
    float cy = rdf(bp + 0x3C);
    wrf(bp + 0x38, cam_x); wrf(bp + 0x60, cam_x); wrf(bg + 0x900AC, cam_x);
    wrf(bp + 0x48, cam_x + w * 0.5f); wrf(bp + 0x50, cam_x + w * 0.5f);
    (void)cy;
}
/* daPlBase_c screen side limit: players may leave (and re-enter) the held screen */
static void title_anim(ppc_context *ctx, uint8_t *base, uint32_t p);
void hle_fn_80055FF0(ppc_context *ctx, uint8_t *base) { title_anim(ctx, base, ctx->r[3]); if (cam_held()) return; orig_fn_80055FF0(ctx, base); }
void hle_fn_80055D00(ppc_context *ctx, uint8_t *base) { if (cam_held()) return; orig_fn_80055D00(ctx, base); }   /* (screen edge push) */
void hle_fn_80055810(ppc_context *ctx, uint8_t *base) { if (cam_held()) return; orig_fn_80055810(ctx, base); }   /* (scroll limits) */
/* While running off (and back in) the title players use the dash animation instead of their
   walk; back at their place they walk again. Animation ids (dPyMdlBase_c): 2 the normal walk
   ("run"), 3 dash ("B_dash"), 4 full-speed dash ("B_dash2"). The game asks for the walk again
   every frame the model shows something else, so while running those requests are dropped. */
#define A_START_ANIM 0x800D5DD0u                   /* dPyMdlBase_c::setAnm (StartAnimOrig)(mdl, id, rate, blend, frame) */
#define ANM_WALK 2
#define ANM_DASH 3
static int anm_dash[8], runin_done[8];
static int title_running(int i) { return cam_held() && (cam_mode == CAM_OUTRUN || cam_mode == CAM_PARKED || (cam_mode == CAM_RUNIN && !runin_done[i])); }
static int title_player_of_model(uint32_t mdl) {
    if (R32(G_GAME_MODE) != MODE_TITLE) return -1;
    for (int i = 0; i < 8; i++) if (title_players[i]) { uint32_t mm = R32(title_players[i] + 3560); if (mm && R32(mm + 4) == mdl) return i; }
    return -1;
}
void hle_StartAnimOrig(ppc_context *ctx, uint8_t *base) {
    int i = title_player_of_model(ctx->r[3]);
    { static int tr = -1; if (tr < 0) tr = getenv("RT_ANMTRACE") != NULL;
      if (tr && i >= 0) rt_log("anm", "f%llu p%d id %d rate %.3f blend %.3f frame %.3f cam %d", (unsigned long long)g_frames_presented, i, (int)ctx->r[4], ctx->f[1].d, ctx->f[2].d, ctx->f[3].d, cam_mode); }
    if (i >= 0 && (int)ctx->r[4] == ANM_WALK) {
        if (title_running(i) && anm_dash[i]) return;              /* keep dashing */
        anm_dash[i] = 0;
    }
    orig_StartAnimOrig(ctx, base);
}
/* start the dash when the running starts (the game only changes animations on its own) */
static void title_anim(ppc_context *ctx, uint8_t *base, uint32_t p) {
    if (R32(G_GAME_MODE) != MODE_TITLE) return;
    int i; for (i = 0; i < 8 && title_players[i] != p; i++) ;
    if (i == 8 || anm_dash[i] || !title_running(i)) return;
    uint32_t mm = R32(p + 3560), mdl = mm ? R32(mm + 4) : 0;            /* dPyMdlMng_c -> dPyMdlBase_c */
    if (!mdl) return;
    static int dash = -1; if (dash < 0) { const char *e = getenv("RT_DASHANM"); dash = e ? atoi(e) : ANM_DASH; }
    ppc_fpr f1 = ctx->f[1], f2 = ctx->f[2], f3 = ctx->f[3];
    ctx->f[1].d = 2.5; ctx->f[2].d = 5.0; ctx->f[3].d = 0.0;              /* rate, blend frames, start frame */
    rt_gcall(ctx, base, A_START_ANIM, 2, mdl, (uint32_t)dash);
    ctx->f[1] = f1; ctx->f[2] = f2; ctx->f[3] = f3;
    anm_dash[i] = 1;
}
/* the title players' walk is fixed by the game each frame; "running" adds the difference to
   their position (their walk animation stays) */
static void set_speed(int i, float target) {
    if (!title_players[i] || target <= WALK) return;
    uint32_t p = title_players[i]; float d = target - WALK;
    wrf(p + 0xAC, rdf(p + 0xAC) + d); wrf(p + 0xB8, rdf(p + 0xB8) + d);
}
/* dGameKey_c::read: the title players read their (demo) remotes; holding 1 makes them run */
static int run_key;
void hle_read__10dGameKey_cFv(ppc_context *ctx, uint8_t *base) {
    uint32_t self = ctx->r[3];
    orig_read__10dGameKey_cFv(ctx, base);
    if (run_key && cam_held()) for (int i = 0; i < 8; i++) { uint32_t c = R32(self + 4 + 4 * i); if (c && !(cam_mode == CAM_RUNIN && runin_done[i])) W32(c + 8, R32(c + 8) | 0x200); }
}
static void cam_freeze(void) {                             /* the list opens */
    if (cam_mode == CAM_FOLLOW) {
        uint32_t bp = R32(G_BG_PARAM); if (!bp) return;
        cam_x = rdf(bp + 0x38);
        for (int i = 0; i < 8; i++) cam_rel[i] = title_players[i] ? rdf(title_players[i] + 0xAC) - cam_x : 0;
        cam_freeze_count = R32(G_STAGE_COUNT);
        rt_log("tdemo", "camera runs on at %.1f (players at +%.1f +%.1f +%.1f +%.1f)", cam_x, cam_rel[0], cam_rel[1], cam_rel[2], cam_rel[3]);
    }
    cam_mode = CAM_OUTRUN;                                /* (reopened while running in: they run off again) */
}
static void cam_return(void) {                             /* Back: run in from the left */
    if (cam_mode != CAM_OUTRUN && cam_mode != CAM_PARKED) return;
    float maxrel = 0; for (int i = 0; i < 8; i++) if (cam_rel[i] > maxrel) maxrel = cam_rel[i];
    for (int i = 0; i < 8; i++) if (title_players[i]) {
        float x = cam_x + cam_rel[i] - maxrel - 48;        /* same formation, just left of the screen */
        wrf(title_players[i] + 0xAC, x); wrf(title_players[i] + 0xB8, x);
        runin_done[i] = 0;
    }
    cam_mode = CAM_RUNIN;
    rt_log("tdemo", "players run back in");
}
#define G_STAGE_TIMER 0x8042A070u                  /* dStageTimer_c::m_instance (P1 0x8042A350) (+4: mTimeValue, 4096 per second) */
static float park_y[8]; static int park_y_set[8];
static uint32_t timer_val; static int timer_held;
static void title_demo_update(void) {
    uint32_t cnt = R32(G_STAGE_COUNT);
    if (cnt < cam_last_count) { cam_mode = CAM_FOLLOW; cam_paused = 0; memset(anm_dash, 0, sizeof anm_dash); }   /* a new title run */
    cam_last_count = cnt;
    float w = screen_w();
    if (cam_mode == CAM_OUTRUN) {                          /* run until past the right edge */
        int off = 1;
        for (int i = 0; i < 8; i++) if (title_players[i]) { set_speed(i, RUN); if (rdf(title_players[i] + 0xAC) < cam_x + w + 24) off = 0; }
        if (off) { cam_mode = CAM_PARKED; rt_log("tdemo", "players off screen (timer %08X: %d)", R32(G_STAGE_TIMER), R32(G_STAGE_TIMER) ? (int)R32(R32(G_STAGE_TIMER) + 4) : -1); }
    }
    if (cam_mode == CAM_PARKED)                            /* kept just past the right edge, out of view */
        for (int i = 0; i < 8; i++) if (title_players[i]) {
            float x = cam_x + w + 40 + i * 24;
            wrf(title_players[i] + 0xAC, x); wrf(title_players[i] + 0xB8, x);
            if (!park_y_set[i]) { park_y[i] = rdf(title_players[i] + 0xB0); park_y_set[i] = 1; }
            wrf(title_players[i] + 0xB0, park_y[i]); wrf(title_players[i] + 0xBC, park_y[i]);   /* (and their height: no pit under them) */
            wrf(title_players[i] + P_SPEEDF, WALK); set_speed(i, WALK);
        }
    else memset(park_y_set, 0, sizeof park_y_set);
    /* the course timer stands still while our menus are up (it ran out after a few minutes in
       Options / Who's playing? / Online: "time up", the players died and the title restarted) */
    {   uint32_t tm = R32(G_STAGE_TIMER);
        if (tm && cam_mode != CAM_FOLLOW) { if (!timer_held) { timer_val = R32(tm + 4); timer_held = 1; } W32(tm + 4, timer_val); }
        else timer_held = 0; }
    if (cam_mode == CAM_RUNIN) {                           /* each slows to a walk at its old place */
        int all = 1;
        for (int i = 0; i < 8; i++) if (title_players[i] && !runin_done[i]) {
            float want = cam_x + cam_rel[i], x = rdf(title_players[i] + 0xAC), d = x - want;
            uint32_t bg = R32(G_BG); float lw = bg ? rdf(bg + 0x8FEA0) : 0;
            if (lw > 100) { if (d < -lw * 0.5f) d += lw; else if (d > lw * 0.5f) d -= lw; }   /* (across the loop) */
            if (d >= 0) { wrf(title_players[i] + 0xAC, want); wrf(title_players[i] + P_SPEEDF, WALK); set_speed(i, WALK); runin_done[i] = 1; }
            else { set_speed(i, RUN); all = 0; }
        }
        if (all) {
            cam_paused += cnt - cam_freeze_count; cam_mode = CAM_FOLLOW;
            rt_log("tdemo", "camera follows again (title timer paused %u frames)", cam_paused);
        }
    }
}
/* system sound filter (SndAudioMgr::startSystemSe): returns the id to play, 0 = none */
uint32_t menu_filter_se(uint32_t id, uint32_t lr) { (void)lr; return id; }
void menu_layout_hook(uint32_t layout, uint32_t root) {
    (void)layout;
    boot_layout_draw(layout, root);
    int kind = layout_kind(root);
    { static int on = -1; if (on < 0) on = getenv("RT_PANETRACE") != NULL;
      if (on && g_frames_presented % 60 == 0) {
          static const char *nm[] = {"N_guideViewL_00", "N_left_00", "N_left_01", "T_world_00", "N_worldBase_00", "N_proportionR_00"};
          char b[600]; int n = 0;
          for (int i = 0; i < 6; i++) { uint32_t p = lyt_find_pane(root, nm[i]); if (p) n += snprintf(b + n, sizeof b - n, " %s a%u/%u f%02X y%.0f gy%.0f", nm[i], R8(p + 0xB8), R8(p + 0xB9), R8(p + 0xBB), rdf(p + 0x30), rdf(p + 0xA0)); }
          if (n) rt_log("pane", "f%llu%s", (unsigned long long)g_frames_presented, b); } }
    if (kind != 1) { gs_layout_draw(layout, root, kind); return; }
    {
        press_pane = lyt_find_pane(root, "T_Press_00"); copy_pane = lyt_find_pane(root, "T_2009nintendo");
        if (!press_pane) return;
        if (title_seen_frame != g_frames_presented) { title_demo_update(); title_trace(); }
        title_seen_frame = g_frames_presented;
        uint8_t a = lyt_get_alpha(press_pane);
        press_ready = (R8(press_pane + 0xBB) & 1) && a >= 128;
        if (!gf.ok && press_ready) font_load(press_pane);
        { static int once; if (!once++) rt_log("menu", "title layout: press %08X (alpha %u flags %02X)", press_pane, a, R8(press_pane + 0xBB)); }
        /* where "Press 2" is, in our 640x480 space (layout: 608x456, origin centre, y up) */
        float gx, gy, fs; uint32_t u;
        u = R32(press_pane + 0x90); memcpy(&gx, &u, 4); u = R32(press_pane + 0xA0); memcpy(&gy, &u, 4);
        u = R32(press_pane + 0xEC); memcpy(&fs, &u, 4);
        if (gx > -400 && gx < 400 && gy > -300 && gy < 300) { press_x = 320 + gx * 640.0f / 608.0f; press_y = 240 - gy * 480.0f / 456.0f; }
        if (fs > 5 && fs < 100) press_size = fs * 480.0f / 456.0f;
        float k = 1;                                   /* fade "Press 2" and the copyright line */
        switch (tstate) {
        case T_PRESS_OUT: k = 1 - fade_t(); break;
        case T_PRESS_IN: k = fade_t(); break;
        case T_IDLE: k = 1; break;
        default: k = 0; break;
        }
        if (inject_until) k = 0;
        if (k < 1) {                                   /* both the pane's alpha and the already-computed global alpha */
            lyt_set_alpha(press_pane, (uint8_t)(a * k)); W8(press_pane + 0xB9, (uint8_t)(R8(press_pane + 0xB9) * k));
            if (copy_pane) { lyt_set_alpha(copy_pane, (uint8_t)(lyt_get_alpha(copy_pane) * k)); W8(copy_pane + 0xB9, (uint8_t)(R8(copy_pane + 0xB9) * k)); }
        }
    }
}

/* the title's own timeout into its demo movie is held while our menus are up
   (fn_2_14800 = the title's check of dScStage frame count > 900 -> opening movie / replay) */
/* players 2-8 get no buttons while our menus (or the title, which player 1 drives) are up */
static int online_overlay(void);
int menu_owns_input(void) { return at_title() || tstate != T_IDLE || ctl_open || inject_until || gs_busy() || online_overlay(); }
int menu_title_busy(void) { return tstate != T_IDLE || inject_until || cam_mode != CAM_FOLLOW; }
void hle_fn_2_14800(ppc_context *ctx, uint8_t *base) {
    if (menu_title_busy()) return;
    uint32_t cnt = R32(G_STAGE_COUNT);
    W32(G_STAGE_COUNT, cnt > cam_paused ? cnt - cam_paused : 0);   /* the held time doesn't count */
    orig_fn_2_14800(ctx, base);
    W32(G_STAGE_COUNT, cnt);
}

/* ================================================================== options
 * Changes apply live where they can (display, window, resolution); nothing is written until
 * Save. Entering takes a snapshot; Cancel (or 1/B) puts everything back. A change that needs a
 * restart (aspect ratio) turns Save into a golden "Save & Restart". */
static const int win_sizes43[][2] = {{0, 0}, {640, 480}, {960, 720}, {1280, 960}, {1600, 1200}, {1920, 1440}};
static const int win_sizes169[][2] = {{0, 0}, {854, 480}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
#define NWIN (int)(sizeof win_sizes43 / sizeof *win_sizes43)
typedef struct { int fullscreen, win, lock, scale, aspect, filter; } optset;
static optset snap, cur;
#define win_sizes (cur.aspect ? win_sizes43 : win_sizes169)     /* sizes for the chosen aspect */
static int win_index_of(int w, int h) { for (int i = 0; i < NWIN; i++) if ((win_sizes43[i][0] == w && win_sizes43[i][1] == h) || (win_sizes169[i][0] == w && win_sizes169[i][1] == h)) return i; return 0; }
static optset read_settings(void) {
    optset o;
    o.fullscreen = plat_get_fullscreen();
    o.win = win_index_of(rt_setting("window_width", 0), rt_setting("window_height", 0));
    o.lock = rt_setting("fixed_window_size", 1);
    o.scale = rt_setting("render_scale", 2);
    o.aspect = rt_setting("aspect_4_3", 1);
    o.filter = rt_setting("texture_filter", 1);
    return o;
}
static void apply_live(const optset *o) {             /* what can change without a restart */
    if (plat_get_fullscreen() != o->fullscreen) plat_set_fullscreen(o->fullscreen);
    int w = (o->aspect ? win_sizes43 : win_sizes169)[o->win][0], h = (o->aspect ? win_sizes43 : win_sizes169)[o->win][1];
    if (!w) { w = rt_widescreen() ? 1280 : 960; h = 720; }
    plat_set_window(w, h, o->lock);
    gx_set_render_scale(o->scale);
    gx_set_filter(o->filter);
}
static int needs_restart(void) { return cur.aspect != snap.aspect; }
enum { O_DISPLAY, O_WINDOW, O_LOCK, O_RES, O_FILTER, O_ASPECT, O_CONTROLS, O_ITEMS, O_CANCEL = O_ITEMS, O_SAVE };   /* last row: Cancel | Save */
static const char *filter_names[6] = {"Nearest", "Bilinear", "Anisotropic 2x", "Anisotropic 4x", "Anisotropic 8x", "Anisotropic 16x"};
static void option_text(int i, char *label, char *value) {
    value[0] = 0;
    switch (i) {
    case O_DISPLAY: strcpy(label, "Display"); strcpy(value, cur.fullscreen ? "Fullscreen" : "Window"); break;
    case O_WINDOW: strcpy(label, "Window Size");
        if (cur.win == 0) strcpy(value, "Auto"); else sprintf(value, "%dx%d", win_sizes[cur.win][0], win_sizes[cur.win][1]); break;
    case O_LOCK: strcpy(label, "Lock Window Size"); strcpy(value, cur.lock ? "On" : "Off"); break;
    case O_RES: strcpy(label, "Resolution"); sprintf(value, "%dx  %dx%d", cur.scale, 640 * cur.scale, 528 * cur.scale); break;
    case O_ASPECT: strcpy(label, "Aspect Ratio"); strcpy(value, cur.aspect ? "4:3" : "16:9"); break;
    case O_FILTER: strcpy(label, "Texture Filtering"); strcpy(value, filter_names[cur.filter < 0 || cur.filter > 5 ? 1 : cur.filter]); break;
    case O_CONTROLS: strcpy(label, "Controls"); break;
    }
}
static void option_change(int i, int dir) {
    switch (i) {
    case O_DISPLAY: cur.fullscreen = !cur.fullscreen; break;
    case O_WINDOW: cur.win = (cur.win + dir + NWIN) % NWIN; break;
    case O_LOCK: cur.lock = !cur.lock; break;
    case O_RES: cur.scale += dir; if (cur.scale < 1) cur.scale = 4; if (cur.scale > 4) cur.scale = 1; break;
    case O_ASPECT: cur.aspect = !cur.aspect; break;
    case O_FILTER: cur.filter = (cur.filter + dir + 6) % 6; break;
    default: return;
    }
    apply_live(&cur);
}
static void options_enter(void) { snap = cur = read_settings(); opt_sel = 0; }
static void options_cancel(void) { cur = snap; apply_live(&cur); rt_log("menu", "options: cancelled"); }
static void options_save(void) {
    rt_setting_set("fullscreen", cur.fullscreen);
    rt_setting_set("window_width", win_sizes[cur.win][0]); rt_setting_set("window_height", win_sizes[cur.win][1]);
    rt_setting_set("fixed_window_size", cur.lock);
    rt_setting_set("render_scale", cur.scale);
    rt_setting_set("aspect_4_3", cur.aspect);
    rt_setting_set("texture_filter", cur.filter);
    rt_log("menu", "options: saved%s", needs_restart() ? ", restarting" : "");
    if (needs_restart()) plat_restart();
}

/* ================================================================== input */
/* the list after "press 2": Singleplayer / Multiplayer / Options; Multiplayer opens a second
   layer: Local / Online */
static int list_layer;                             /* 0 the main list, 1 Multiplayer: Local / Online */
static const char *list0[] = {"Singleplayer", "Multiplayer", "Options"}, *list1[] = {"Local", "Online"};
#define LIST_N (list_layer ? 2 : 3)
static const char *list_item(int i) { return list_layer ? list1[i] : list0[i]; }
static int online_play;                            /* the host went on from Multiplayer > Online: the start is at the character select */

int gs_char_select_ready(void);
/* the host's start of the online game: over the character select, until the online game runs */
static int online_overlay(void) {
    int on = online_play && net_role() == NET_HOST && !session_on() && gs_char_select_ready();
    static int was; if (on != was) { was = on; rt_log("menu", "online start %s", on ? "shown (character select)" : "hidden"); }
    return on;
}
static void start_game(int single) {
    rt_log("menu", "%s: continue into the game", single ? "singleplayer" : "multiplayer");
    play_mode = single ? 1 : 2;
    set_state(T_IDLE);
    inject_t0 = rt_emu_ns(); inject_until = inject_t0 + 4000000000ull;
}

static uint32_t filter_buttons(uint32_t b);
static int controls_input(uint32_t pressed);
static void controls_reset(void);
static void online_enter(void);
static void profiles_enter(void);
static int profiles_input(void);
static void profiles_draw(float a);
static int online_input(uint32_t pressed);
static void online_draw(float a);
/* A button the game didn't get when it was pressed (our menus had it, or input was held during
   a screen change) stays hidden from the game until it is let go - otherwise a press that lasts
   past the end of a menu or a fade reaches the game as a new press (going back from the title
   menu with 1 used to start the game this way, if 1 was still held when the menu had faded). */
uint32_t menu_filter_buttons(uint32_t b) {
    static uint32_t hidden;
    uint32_t out = filter_buttons(b);
    hidden = (hidden & b) | (b & ~out);
    return out & ~hidden;
}
static uint32_t filter_buttons(uint32_t b) {
    uint32_t pressed = b & ~prev_btn; prev_btn = b;
    {   int boot_kb_active(void); void boot_kb_press(void);              /* the boot's keyboard screen (rt_bootscreen.c): 2 goes on */
        if (boot_kb_active()) { if (pressed & (B_2 | B_A)) boot_kb_press(); return 0; } }
    if (update_screen_active()) { if (pressed & (B_1 | B_B)) update_skip(); return 0; }   /* the update screen (boot) */
    uint64_t now = rt_emu_ns();
    { static int tr = -1; if (tr < 0) tr = getenv("RT_MENUTRACE") != NULL;
      if (tr && g_frames_presented % 60 == 0) rt_log("menutrace", "scene %u mode %u title_seen %llu (frame %llu) press_ready %d tstate %d",
          R16(G_NOW_SCENE), R32(G_GAME_MODE), (unsigned long long)title_seen_frame, (unsigned long long)g_frames_presented, press_ready, (int)tstate); }
    int scene = R16(G_NOW_SCENE);
    {   /* back at the title (for a second: it shows a few more frames while the game leaves it) */
        static uint64_t live_since;
        if (title_live() && !inject_until && tstate == T_IDLE) { if (!live_since) live_since = g_frames_presented; }
        else live_since = 0;
        if (live_since && g_frames_presented - live_since > 60 && (play_mode || online_play)) {
            rt_log("menu", "back at the title (frame %llu)", (unsigned long long)g_frames_presented); play_mode = 0; online_play = 0; } }
    if (inject_until) {                                     /* replay "2" until the game leaves the title */
        if (now > inject_until || !at_title()) { inject_until = 0; title_seen_frame = 0; return 0; }
        return ((now - inject_t0) / 1000000ull) % 250 < 100 ? B_2 : 0;
    }
    if (ctl_open) {                                         /* the Controls screen over the character select */
        if (scene != SCENE_GAME_SETUP) { ctl_open = 0; controls_reset(); }
        else {
            if (now_s() - ctl_t > FADE * 0.6 && controls_input(pressed)) { ctl_open = 0; ctl_hold = b; }
            return 0;
        }
    }
    if (ctl_hold) { ctl_hold &= b; b &= ~ctl_hold; }        /* the button that closed it reaches the game only once let go */
    if (online_overlay()) {                                 /* host, character select: 2 starts the online game */
        if (net_online_starting()) return 0;
        if ((pressed & (B_2 | B_A)) && net_online_players() > 0) { net_start_session(); return 0; }
        return b & (B_1 | B_B | B_HOME);                    /* (1: back to the file select) */
    }
    b = gs_filter_buttons(b);                               /* game setup screens (rt_gamesetup.c) */
    if (!title_live()) {                                    /* left the title (demo, reset...): drop our menus */
        if (tstate == T_CONTROLS) controls_reset();
        if (tstate != T_IDLE) set_state(T_IDLE);
        cam_mode = CAM_FOLLOW;
        return at_title() ? b & B_HOME : b;                 /* (title still loading: nothing for the game but HOME) */
    }
    { static long at = -2; if (at == -2) { const char *e = getenv("RT_CONTROLS_AT"); at = e ? atol(e) : -1; }   /* testing: open Controls */
      if (at >= 0 && (long)g_frames_presented >= at && tstate == T_IDLE) { at = -1; set_state(T_CONTROLS); return 0; } }
    switch (tstate) {
    case T_IDLE:
        {   static int t = -1; if (t < 0) t = getenv("RT_MENU_ONLINE") != NULL;   /* testing: straight to the Online screen */
            if (t && press_ready && tstate == T_IDLE) { t = 0; cam_freeze(); set_state(T_ONLINE); online_enter(); return 0; } }
        {   static int t = -1; if (t < 0) t = getenv("RT_NET_ONLINE_SETUP") != NULL;   /* testing: the host goes on to the setup screens once someone joined */
            if (t && press_ready && net_role() == NET_HOST && net_online_players() > 0) { t = 0; cam_freeze(); set_state(T_ONLINE); online_enter(); return 0; } }
        /* 2 (or A) opens our list. The game's title would take any button as "start" and go
           straight to the setup screens, so it gets none (but HOME). */
        if ((pressed & (B_2 | B_A)) && press_ready) { set_state(T_PRESS_OUT); next_action = NEXT_NONE; sel = 0; list_layer = 0; cam_freeze(); return 0; }
        return b & B_HOME;
    case T_PRESS_OUT:
        if (fade_t() >= 1) set_state(T_LIST);
        return 0;
    case T_PRESS_IN:
        if (fade_t() >= 1) set_state(T_IDLE);
        return 0;
    case T_LIST_OUT:
        if (fade_t() >= 1) {
            if (next_action == NEXT_SINGLE) start_game(1);
            else if (next_action == NEXT_MULTI) start_game(0);
            else if (next_action == NEXT_OPTIONS) { set_state(T_OPTIONS); options_enter(); }
            else if (next_action == NEXT_ONLINE) { set_state(T_ONLINE); online_enter(); }
            else set_state(T_PRESS_IN);
        }
        return 0;
    case T_LIST:
        if (fade_t() < 0.6f) return 0;
        if (pressed & NAV_UP) sel = (sel + LIST_N - 1) % LIST_N;
        if (pressed & NAV_DOWN) sel = (sel + 1) % LIST_N;
        if (sel >= LIST_N) sel = 0;
        if (pressed & (B_2 | B_A)) {
            if (list_layer == 0 && sel == 1) { set_state(T_PROFILES); profiles_enter(); }   /* Multiplayer: Who's playing?, then Local / Online */
            else { next_action = list_layer ? (sel == 0 ? NEXT_MULTI : NEXT_ONLINE) : sel == 0 ? NEXT_SINGLE : NEXT_OPTIONS; set_state(T_LIST_OUT); }
        } else if (pressed & (B_1 | B_B)) {
            if (list_layer) { set_state(T_PROFILES); profiles_enter(); }                   /* (back to Who's playing?) */
            else { next_action = NEXT_TITLE; set_state(T_LIST_OUT); cam_return(); }
        }
        return 0;
    case T_OPTIONS:
        if (fade_t() < 0.6f) return 0;
        if (opt_sel < O_ITEMS) {
            if (pressed & NAV_UP) opt_sel = opt_sel == 0 ? O_SAVE : opt_sel - 1;
            if (pressed & NAV_DOWN) opt_sel = opt_sel == O_ITEMS - 1 ? O_SAVE : opt_sel + 1;
            if (opt_sel == O_CONTROLS) { if (pressed & (B_2 | B_A | NAV_RIGHT)) { set_state(T_CONTROLS); return 0; } }
            else if (pressed & (NAV_LEFT | NAV_RIGHT)) option_change(opt_sel, (pressed & NAV_LEFT) ? -1 : 1);
            else if (pressed & (B_2 | B_A)) option_change(opt_sel, 1);
        } else {                                            /* Cancel | Save row */
            if (pressed & NAV_UP) opt_sel = O_ITEMS - 1;
            if (pressed & NAV_DOWN) opt_sel = 0;
            if (pressed & NAV_LEFT) opt_sel = O_CANCEL;
            if (pressed & NAV_RIGHT) opt_sel = O_SAVE;
            if (pressed & (B_2 | B_A)) {
                if (opt_sel == O_SAVE) options_save(); else options_cancel();
                set_state(T_LIST); list_layer = 0; sel = 2;
                return 0;
            }
        }
        if (pressed & (B_1 | B_B)) { options_cancel(); set_state(T_LIST); list_layer = 0; sel = 2; }
        return 0;
    case T_CONTROLS:
        if (fade_t() < 0.6f) return 0;
        if (controls_input(pressed)) { set_state(T_OPTIONS); t_state -= FADE; }
        return 0;
    case T_PROFILES:
        if (fade_t() < 0.6f) { while (plat_text_char()) {} return 0; }
        {   int r = profiles_input();
            if (r == 1) { set_state(T_LIST); list_layer = 0; sel = 1; }
            else if (r == 2) { set_state(T_LIST); list_layer = 1; sel = 0; t_state -= FADE * 0.6; }
        }
        return 0;
    case T_ONLINE:
        if (fade_t() < 0.6f) { while (plat_text_char()) {} return 0; }
        {   int r = online_input(pressed);
            if (r == 1) { set_state(T_LIST); list_layer = 1; sel = 1; }
            else if (r == 2) { online_play = 1; start_game(0); }   /* host: on to the game select / file select */
        }
        return 0;
    }
    return 0;
}

/* ================================================================== drawing */
static uint32_t with_alpha(uint32_t rgb, float a) { int v = (int)(a * 255); if (v < 0) v = 0; if (v > 255) v = 255; return (rgb & 0xFFFFFF00u) | (uint32_t)v; }
/* ---- Controls screen (Options > Controls, or "Settings +" on the character select) ----
   The eight player slots: each one's lights, its controller (the keyboard, a Wii Remote, or
   none - Left/Right changes it, swapping with the slot that had that controller; rt_remotes.c
   keeps the choice) and whether it's connected. "Pair Wii Remotes" looks for remotes whose red
   SYNC button was pressed and pairs them with the PC for good (plat_wm_pair); a newly paired
   remote takes the first free slot. 2 on a remote's slot forgets it (asks first). */
enum { CR_ROWS = 8, CR_PAIR = 8 };
static int ctl_sel, ctl_confirm = -1, ctl_pairing;
static void controls_reset(void) { ctl_sel = 0; ctl_confirm = -1; if (ctl_pairing) { plat_wm_pair(0); ctl_pairing = 0; } }
/* the next (dir +1) or previous (-1) controller for slot s */
static void controls_cycle(int s, int dir) {
    int kinds[24]; uint64_t addrs[24];
    int n = remotes_choices(kinds, addrs, 24);
    int kind, conn, player; uint64_t addr;
    remotes_slot_info(s, &kind, &addr, &conn, &player);
    int cur = 0;
    for (int i = 0; i < n; i++) if (kinds[i] == kind && (kind != REMOTES_REMOTE || addrs[i] == addr)) cur = i;
    for (int step = 1; step < n; step++) {
        int i = ((cur + dir * step) % n + n) % n;
        if (remotes_assign(s, kinds[i], addrs[i])) { rt_log("menu", "controls: player slot %d -> choice %d/%d", s + 1, i, n); return; }
    }
}
/* returns 1 when the screen closes */
static int controls_input(uint32_t pressed) {
    if (ctl_confirm >= 0) {                                 /* "Forget this Wii Remote?" */
        if (pressed & (B_2 | B_A)) { remotes_forget(ctl_confirm); ctl_confirm = -1; }
        else if (pressed & (B_1 | B_B)) ctl_confirm = -1;
        return 0;
    }
    if (pressed & NAV_UP) ctl_sel = ctl_sel == 0 ? CR_PAIR : ctl_sel - 1;
    if (pressed & NAV_DOWN) ctl_sel = ctl_sel == CR_PAIR ? 0 : ctl_sel + 1;
    if (ctl_sel < CR_ROWS && (pressed & (NAV_LEFT | NAV_RIGHT))) controls_cycle(ctl_sel, (pressed & NAV_LEFT) ? -1 : 1);
    if (pressed & (B_2 | B_A)) {
        if (ctl_sel == CR_PAIR) {
            if (!plat_wm_pair_supported()) return 0;
            ctl_pairing = !ctl_pairing; plat_wm_pair(ctl_pairing);
            rt_log("menu", "controls: pairing %s", ctl_pairing ? "on" : "off");
        } else {
            int kind, conn, player; uint64_t addr;
            remotes_slot_info(ctl_sel, &kind, &addr, &conn, &player);
            if (kind == REMOTES_REMOTE) ctl_confirm = ctl_sel;
        }
    }
    if (pressed & (B_1 | B_B)) { controls_reset(); return 1; }
    return 0;
}
static void addr_str(uint64_t a, char *s) {
    sprintf(s, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)(a >> 40) & 0xFF, (unsigned)(a >> 32) & 0xFF, (unsigned)(a >> 24) & 0xFF,
            (unsigned)(a >> 16) & 0xFF, (unsigned)(a >> 8) & 0xFF, (unsigned)a & 0xFF);
}
static int ctl_bg RT_LOCAL; static int ctl_bg_tried RT_LOCAL;
static void controls_draw(float a) {
    if (!ctl_bg_tried) {
        ctl_bg_tried = 1;
        char p[1200]; int w, h;
        snprintf(p, sizeof p, "%s/ui/%s", g_root, rt_widescreen() ? "controls_bg_169.png" : "controls_bg_43.png");
        uint32_t *px = ui_load_png(p, &w, &h);
        if (px) { ctl_bg = ui_register_texture(px, w, h); free(px); }
        else rt_log("menu", "controls: no background %s", p);
    }
    float fx = rt_widescreen() ? -320.0f / 3 : 0, fw = rt_widescreen() ? 640 + 640.0f / 3 : 640;   /* whole picture */
    if (ctl_bg) ui_image(ctl_bg, fx, 0, fw, 480, 0, 0, 1, 1, with_alpha(0xFFFFFFFFu, a));
    else ui_rect(fx, 0, fw, 480, with_alpha(0x6EC8F0FFu, a));
    ui_rect(fx, 22, fw, 56, with_alpha(0x10182800u, a * 0.55f));          /* title band */
    gtext_centered(320, 32, 34, "Controls", with_alpha(0xFFFFFFFFu, a));
    const uint32_t WHITE = 0xFFFFFFFFu, GOLD = 0xFFC830FFu, GREEN = 0x80FF80FFu, GREY = 0xA0A8B8FFu;
    float x = 40, y = 92, w = 560, h = 360;
    ui_rect(x - 3, y - 3, w + 6, h + 6, with_alpha(0xF0C03000u, a));
    ui_rect(x, y, w, h, with_alpha(0x101828D8u, a * 0.85f));
    gtext(x + 18, y + 8, 22, "Players", with_alpha(GOLD, a));
    float ry = y + 38, rh = 25, size = 19;
    double pulse = 0.5 + 0.5 * sin(now_s() * 5.0);
    for (int i = 0; i < CR_ROWS; i++, ry += rh) {
        int kind, conn, player; uint64_t addr;
        remotes_slot_info(i, &kind, &addr, &conn, &player);
        int sel = i == ctl_sel;
        if (sel) ui_rect(x + 8, ry - 3, w - 16, rh - 1, with_alpha(0xFFC83000u, a * (0.25f + 0.15f * (float)pulse)));
        char t[64];
        int shown = conn && player >= 0 ? player : i;      /* the game's player number once it's in */
        snprintf(t, sizeof t, "Player %d", shown + 1);
        gtext(x + 22, ry, size, t, with_alpha(conn ? WHITE : GREY, a));
        uint8_t l = remotes_lights_for(shown);              /* its lights */
        for (int k = 0; k < 4; k++) {
            float lx = x + 122 + k * 15, ly = ry + 6;
            ui_rect(lx, ly, 10, 8, with_alpha(0x000000FFu, a * 0.6f));
            if (l & (1 << k)) ui_rect(lx + 1, ly + 1, 8, 6, with_alpha(kind == REMOTES_REMOTE && conn ? 0x40C8FFFFu : 0x406080FFu, a));
        }
        if (ctl_confirm == i) { gtext(x + 200, ry, size, "Forget this Wii Remote?  2: Yes   1: No", with_alpha(GOLD, a)); continue; }
        float dx = x + 206;
        if (sel) { gtext(x + 192, ry, size, "<", with_alpha(GOLD, a)); }
        const char *dev = kind == REMOTES_KEYBOARD ? "Keyboard" : kind == REMOTES_REMOTE ? "Wii Remote" : "None";
        gtext(dx, ry, size, dev, with_alpha(kind == REMOTES_NONE ? GREY : WHITE, a));
        float ex = dx + gtext_width(dev, size);
        if (kind == REMOTES_REMOTE && addr) { addr_str(addr, t); gtext(ex + 8, ry + 3, 14, t, with_alpha(GREY, a)); ex += 8 + gtext_width(t, 14); }
        if (sel) gtext(ex + 6, ry, size, ">", with_alpha(GOLD, a));
        if (kind == REMOTES_REMOTE) {
            const char *st = conn ? "Connected" : "Not connected";
            gtext(x + w - 22 - gtext_width(st, 16), ry + 2, 16, st, with_alpha(conn ? GREEN : GREY, a));
        }
    }
    /* Pair Wii Remotes */
    float by = ry + 6, bw = 260, bx = 320 - bw / 2;
    int on = ctl_sel == CR_PAIR;
    ui_rect(bx - 2, by - 2, bw + 4, 30, with_alpha(on ? 0xFFC830FFu : 0x506078FFu, a));
    ui_rect(bx, by, bw, 26, with_alpha(ctl_pairing ? 0x206030FFu : 0x203048FFu, a));
    gtext_centered(320, by + 3, 20, ctl_pairing ? "Stop Pairing" : "Pair Wii Remotes", with_alpha(WHITE, a));
    char msg[200] = "";
    if (!plat_wm_pair_supported()) snprintf(msg, sizeof msg, "Pairing needs this PC's Bluetooth (Windows)");
    else if (ctl_pairing) { int st = plat_wm_pair_state(msg, sizeof msg); if (!msg[0]) snprintf(msg, sizeof msg, st ? "Looking for Wii Remotes..." : "Starting..."); }
    else snprintf(msg, sizeof msg, "Pair once with the red SYNC button; after that any button reconnects");
    gtext_centered(320, by + 34, 15, msg, with_alpha(ctl_pairing ? GOLD : GREY, a));
    uint64_t un[8]; int nun = remotes_unassigned(un, 8);
    if (nun) {
        char u[120]; snprintf(u, sizeof u, "%d connected Wii Remote%s not in a player slot - choose with Left/Right", nun, nun > 1 ? "s" : "");
        gtext_centered(320, by + 54, 15, u, with_alpha(GOLD, a));
    }
    gtext_centered(320, y + h - 22, 14, "Keyboard: arrows move, X jump (2), Z run (1), C shake, Enter +, Shift -, H Home",
                   with_alpha(GREY, a));
    const char *hint = ctl_confirm >= 0 ? "2: Yes   1: No" : ctl_sel == CR_PAIR ? "2: Select   1: Back" : "Left/Right: Change   2: Forget   1: Back";
    gtext_centered(320, 456, 20, hint, with_alpha(WHITE, a));
}
/* ---- Online screen (title list > Online): host a game, or join one by its address ----
   Online play is host-authoritative (rt_net.c, docs/ONLINE.md): the host's game is the real one;
   phase 1 plays the joined players' controllers in the host's player slots. Rows:
     Host a game / Stop hosting      Host port (settings.ini net_port: the UDP port forwarded)
     Choose the game and save file   Join: the host's address    Join port (the host's port)
     Leave
   Typing uses the PC keyboard (2 / Enter starts typing, Enter keeps it, Esc stops); a port
   row also takes Left/Right (one up / down). Kept in online.ini (next to settings.ini):
   name = <your name>, join = <last address>, join_port = <its port>. */
enum { ON_HOST, ON_HPORT, ON_START, ON_JOIN, ON_JPORT, ON_LEAVE, ON_ROWS };
#define DEFAULT_PORT 41790
static int on_sel, on_edit RT_LOCAL;                      /* (on_edit: the row being typed in + 1, 0 = none) */
static char on_addr[64] RT_LOCAL, on_typed[8] RT_LOCAL;
static int on_jport RT_LOCAL = DEFAULT_PORT;
static int port_ok(int p) { return p >= 1 && p <= 65535; }
/* "203.0.113.5:41790" typed (or kept by an older version) as the address: the port goes to its row */
static void split_addr_port(void) {
    char *c = strrchr(on_addr, ':');
    if (c && strchr(on_addr, ':') == c) { int p = atoi(c + 1); if (port_ok(p)) on_jport = p; *c = 0; }
}
static void online_ini(int save) {
    char p[1200]; snprintf(p, sizeof p, "%s/online.ini", g_root);
    if (!save) {
        FILE *f = fopen(p, "r"); if (!f) return;
        char line[128]; while (fgets(line, sizeof line, f)) {
            char v[64]; int n;
            if (sscanf(line, " join_port = %d", &n) == 1) { if (port_ok(n)) on_jport = n; }
            else if (sscanf(line, " join = %63[^\r\n ]", v) == 1) snprintf(on_addr, sizeof on_addr, "%s", v);
        }
        fclose(f); split_addr_port(); return;
    }
    /* keep the other lines (name = ...) */
    char keep[2048] = ""; FILE *f = fopen(p, "r");
    if (f) { char line[128]; while (fgets(line, sizeof line, f)) if (!strstr(line, "join") && strlen(keep) + strlen(line) < sizeof keep - 1) strcat(keep, line); fclose(f); }
    if (!strstr(keep, "name")) strcat(keep, "# Online play (title > Online). name = your name the others see.\nname = Player\n");
    f = fopen(p, "w"); if (!f) return;
    fprintf(f, "%sjoin = %s\njoin_port = %d\n", keep, on_addr, on_jport); fclose(f);
}
static void online_enter(void) { on_sel = session_on() ? ON_LEAVE : net_role() == NET_HOST ? ON_START : net_role() == NET_OFF ? ON_HOST : ON_LEAVE; on_edit = 0; online_ini(0); while (plat_text_char()) {} }
static int online_port(void) { int p = rt_setting("net_port", DEFAULT_PORT); return port_ok(p) ? p : DEFAULT_PORT; }
static int host_port_locked(void) { return net_role() == NET_HOST; }   /* (players joined on this one) */
static void set_host_port(int p) { if (port_ok(p) && p != online_port()) { rt_setting_set("net_port", p); rt_log("menu", "online: host port %d", p); } }
static void set_join_port(int p) { if (port_ok(p) && p != on_jport) { on_jport = p; online_ini(1); } }
static void join_now(void) {
    split_addr_port(); online_ini(1);
    char a[80]; snprintf(a, sizeof a, "%s:%d", on_addr, on_jport);
    net_join(a);
}
static int online_input(uint32_t pressed) {
    if (on_edit) {                                          /* typing: the keyboard's characters only */
        int row = on_edit - 1, c;
        while ((c = plat_text_char())) {
            if (row == ON_JOIN) {
                size_t n = strlen(on_addr);
                if (c == 8) { if (n) on_addr[n - 1] = 0; }
                else if (c == 13) { on_edit = 0; join_now(); }
                else if (c == 27) { on_edit = 0; split_addr_port(); }
                else if (((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == ':' || c == '-') && n < sizeof on_addr - 1) { on_addr[n] = (char)c; on_addr[n + 1] = 0; }
            } else {                                        /* a port: digits */
                size_t n = strlen(on_typed);
                if (c == 8) { if (n) on_typed[n - 1] = 0; }
                else if (c == 13) { int p = atoi(on_typed); on_edit = 0; if (row == ON_HPORT) set_host_port(p); else set_join_port(p); }
                else if (c == 27) on_edit = 0;
                else if (c >= '0' && c <= '9' && n < 5) { on_typed[n] = (char)c; on_typed[n + 1] = 0; }
            }
        }
        return 0;
    }
    while (plat_text_char()) {}
    if (session_on()) {                                     /* in the online game: only Leave (the host's choice, on every PC) */
        on_sel = ON_LEAVE;
        if (pressed & (B_2 | B_A)) net_leave_session();
        return (pressed & (B_1 | B_B)) != 0;
    }
    if (pressed & NAV_UP) on_sel = (on_sel + ON_ROWS - 1) % ON_ROWS;
    if (pressed & NAV_DOWN) on_sel = (on_sel + 1) % ON_ROWS;
    if (pressed & (NAV_LEFT | NAV_RIGHT)) {                 /* a port row: one up / down */
        int d = (pressed & NAV_RIGHT) ? 1 : -1;
        if (on_sel == ON_HPORT && !host_port_locked()) { int p = online_port() + d; set_host_port(p < 1 ? 65535 : p > 65535 ? 1 : p); }
        if (on_sel == ON_JPORT) { int p = on_jport + d; set_join_port(p < 1 ? 65535 : p > 65535 ? 1 : p); }
    }
    if (pressed & (B_2 | B_A)) {
        if (on_sel == ON_HOST) { if (net_role() == NET_HOST) net_leave(); else net_host(online_port()); }
        else if (on_sel == ON_HPORT) { if (!host_port_locked()) { on_edit = ON_HPORT + 1; on_typed[0] = 0; } }
        else if (on_sel == ON_START) { if (net_role() == NET_HOST) return 2; }   /* (host: on to the setup screens) */
        else if (on_sel == ON_JOIN) on_edit = ON_JOIN + 1;
        else if (on_sel == ON_JPORT) { on_edit = ON_JPORT + 1; on_typed[0] = 0; }
        else if (on_sel == ON_LEAVE) net_leave();
    }
    if (pressed & (B_1 | B_B)) return 1;                    /* (the game stays connected) */
    return 0;
}
static void online_draw(float a) {
    float fx = rt_widescreen() ? -320.0f / 3 : 0, fw = rt_widescreen() ? 640 + 640.0f / 3 : 640;
    if (!ctl_bg_tried) {
        ctl_bg_tried = 1;
        char p[1200]; int w, h;
        snprintf(p, sizeof p, "%s/ui/%s", g_root, rt_widescreen() ? "controls_bg_169.png" : "controls_bg_43.png");
        uint32_t *px = ui_load_png(p, &w, &h);
        if (px) { ctl_bg = ui_register_texture(px, w, h); free(px); }
    }
    if (ctl_bg) ui_image(ctl_bg, fx, 0, fw, 480, 0, 0, 1, 1, with_alpha(0xFFFFFFFFu, a));
    else ui_rect(fx, 0, fw, 480, with_alpha(0x6EC8F0FFu, a));
    ui_rect(fx, 22, fw, 56, with_alpha(0x10182800u, a * 0.55f));
    gtext_centered(320, 32, 34, "Online", with_alpha(0xFFFFFFFFu, a));
    const uint32_t WHITE = 0xFFFFFFFFu, GOLD = 0xFFC830FFu, GREEN = 0x80FF80FFu, GREY = 0xA0A8B8FFu;
    float x = 40, y = 92, w = 560, h = 360;
    ui_rect(x - 3, y - 3, w + 6, h + 6, with_alpha(0xF0C03000u, a));
    ui_rect(x, y, w, h, with_alpha(0x101828D8u, a * 0.85f));
    double pulse = 0.5 + 0.5 * sin(now_s() * 5.0);
    int role = net_role(), blink = on_edit && ((int)(now_s() * 2) & 1);
    char t[160];
    float ry = y + 12, rh = 27;
    for (int i = 0; i < ON_ROWS; i++, ry += rh) {
        uint32_t c = WHITE;
        if (session_on()) {                                 /* (the online game is running: just Leave) */
            if (i != ON_LEAVE) continue;
            snprintf(t, sizeof t, "Leave the online game");
        } else if (i == ON_HOST) snprintf(t, sizeof t, role == NET_HOST ? "Stop hosting" : "Host a game");
        else if (i == ON_HPORT) {
            if (on_edit == ON_HPORT + 1) { snprintf(t, sizeof t, "    Host port:  %s%s", on_typed, blink ? "_" : ""); c = GOLD; }
            else { snprintf(t, sizeof t, i == on_sel && !host_port_locked() ? "    Host port:  <  %d  >" : "    Host port:  %d", online_port()); if (host_port_locked()) c = GREY; }
        }
        else if (i == ON_START) { snprintf(t, sizeof t, "Choose the game and save file"); if (role != NET_HOST) c = GREY; }
        else if (i == ON_JOIN) {
            snprintf(t, sizeof t, "Join:  %s%s", on_addr[0] || on_edit == ON_JOIN + 1 ? on_addr : "(2 to type the host's address)", on_edit == ON_JOIN + 1 && blink ? "_" : "");
            if (on_edit == ON_JOIN + 1) c = GOLD;
        } else if (i == ON_JPORT) {
            if (on_edit == ON_JPORT + 1) { snprintf(t, sizeof t, "    Join port:  %s%s", on_typed, blink ? "_" : ""); c = GOLD; }
            else snprintf(t, sizeof t, i == on_sel ? "    Join port:  <  %d  >" : "    Join port:  %d", on_jport);
        } else { snprintf(t, sizeof t, "Leave"); if (role == NET_OFF) c = GREY; }
        if (i == on_sel) ui_rect(x + 8, ry - 4, w - 16, rh - 2, with_alpha(0xFFC83000u, a * (0.25f + 0.15f * (float)pulse)));
        gtext(x + 24, ry, 20, t, with_alpha(c, a));
    }
    /* status and who's in (two columns) */
    gtext(x + 24, ry + 4, 16, net_status()[0] ? net_status() : session_on() ? "Online game" : "Not connected", with_alpha(role == NET_OFF && !session_on() ? GREY : GREEN, a));
    ry += 30;
    char nm[48]; int slot, ping;
    for (int i = 0; i < 8 && net_lobby(i, nm, sizeof nm, &slot, &ping); i++) {
        float cx = x + 24 + (i & 1) * (w / 2), cy = ry + (i / 2) * 20;
        snprintf(t, sizeof t, "P%d", slot + 1);
        gtext(cx, cy, 17, t, with_alpha(WHITE, a));
        gtext(cx + 40, cy, 17, nm, with_alpha(WHITE, a));
        if (ping >= 0) { snprintf(t, sizeof t, "%d ms", ping); gtext(cx + w / 2 - 48 - gtext_width(t, 15), cy + 2, 15, t, with_alpha(GREY, a)); }
    }
    char tipb[160];
    const char *tip = tipb;
    if (session_on()) tip = "Everyone plays the same game. Leaving ends it: everyone goes back to their own game.";
    else if (role == NET_HOST) snprintf(tipb, sizeof tipb, "Players join with your public IP and port %d (UDP, forwarded on your router).", online_port());
    else if (role == NET_CLIENT) tip = "Joined: you'll be in the host's character select when it starts the game.";
    else if (on_sel == ON_HPORT || on_sel == ON_JPORT) tip = "A port: Left / Right changes it, 2 types it (1024-65535 is best).";
    else tip = "Host: forward the host port (UDP) on your router.  Join: the host's IP and port.";
    gtext_centered(320, y + h - 24, 14, tip, with_alpha(GREY, a));
    const char *keys = !on_edit ? "2: Select   1: Back" : on_edit == ON_JOIN + 1 ? "Type the address   Enter: Join   Esc: Stop typing" : "Type the port   Enter: Keep it   Esc: Cancel";
    gtext_centered(320, 456, 20, keys, with_alpha(WHITE, a));
}
/* ---- Who's playing? (Multiplayer, before Local / Online): a profile per controller ----
   Each connected controller drives its own row (rt_remotes.c: remotes_slot_raw): Left/Right
   picks one of the profiles nobody else has, "New profile" or "Guest"; 2 takes it. A new
   profile asks for a username (1-4 letters / digits: Up/Down change the letter, Left/Right
   move; the keyboard can type it) and a preferred character (Left/Right), then 2. + on a
   profile edits it. When every row is ready, player 1's 2 goes on (to Local / Online); player
   1's 1 goes back. rt_profiles.c keeps the profiles and which controller used which. */
enum { PR_CHOOSE, PR_NAME, PR_CHAR, PR_READY };
typedef struct { int active, st, choice, edit, cur, pref, err; char name[5]; uint32_t prev; } prow_t;
static prow_t PR[8];
static const char NAME_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
#define CH_NEW (-2)
#define CH_GUEST (-3)
static int prof_taken(int i, int me) { for (int s = 0; s < 8; s++) if (s != me && PR[s].active && PR[s].st == PR_READY && PR[s].choice == i) return 1; return 0; }
/* the next choice (dir +1 / -1): free profiles, then New, then Guest */
static int next_choice(int me, int cur, int dir) {
    int n = prof_count(), total = n + 2, pos = cur >= 0 ? cur : cur == CH_NEW ? n : n + 1;
    for (int k = 0; k < total; k++) {
        pos = (pos + dir + total) % total;
        int c = pos < n ? pos : pos == n ? CH_NEW : CH_GUEST;
        if (c < 0 || !prof_taken(c, me)) return c;
    }
    return CH_NEW;
}
static int slot_connected(int s) { int kind, conn, pl; uint64_t a; remotes_slot_info(s, &kind, &a, &conn, &pl); return conn || s == 0; }
static void row_init(int s) {
    prow_t *r = &PR[s]; memset(r, 0, sizeof *r); r->active = 1; r->edit = -1; r->pref = 0;
    r->prev = 0xFFFFFFFFu;                              /* (a button held from the list isn't a press) */
    int remembered = prof_of_slot(s) >= 0 ? prof_of_slot(s) : prof_of_controller(s);
    r->choice = remembered >= 0 && !prof_taken(remembered, s) ? remembered : next_choice(s, CH_GUEST, 1);
}
static void profiles_enter(void) {
    for (int s = 0; s < 8; s++) { PR[s].active = 0; if (slot_connected(s)) row_init(s); }
    rt_log("menu", "who's playing (%d profiles)", prof_count());
    while (plat_text_char()) {}
}
static int profiles_input(void) {
    int all_ready = 1, any = 0;
    for (int s = 0; s < 8; s++) {
        int conn = slot_connected(s);
        if (conn && !PR[s].active) row_init(s);
        if (!conn) PR[s].active = 0;
        if (!PR[s].active) continue;
        any = 1;
        prow_t *r = &PR[s];
        uint32_t b = remotes_slot_raw(s), pressed = r->prev == 0xFFFFFFFFu ? 0 : b & ~r->prev; r->prev = b;
        int is_kb;
        { int kind, conn, pl; uint64_t ad; remotes_slot_info(s, &kind, &ad, &conn, &pl); is_kb = kind == REMOTES_KEYBOARD || (s == 0 && !conn); }
        switch (r->st) {
        case PR_CHOOSE:
            if (r->choice >= 0 && prof_taken(r->choice, s)) r->choice = next_choice(s, r->choice, 1);
            if (pressed & NAV_LEFT) r->choice = next_choice(s, r->choice, -1);
            if (pressed & NAV_RIGHT) r->choice = next_choice(s, r->choice, 1);
            if (pressed & (B_2 | B_A)) {
                if (r->choice == CH_NEW) { r->st = PR_NAME; r->edit = -1; memset(r->name, 0, sizeof r->name); r->name[0] = 'A'; r->cur = 0; r->pref = 0; r->err = 0; }
                else r->st = PR_READY;
            } else if ((pressed & B_PLUS) && r->choice >= 0) {          /* edit this profile */
                r->st = PR_NAME; r->edit = r->choice; snprintf(r->name, sizeof r->name, "%s", prof_name(r->choice)); r->cur = 0;
                r->pref = chars_index(prof_pref(r->choice)); if (r->pref < 0) r->pref = 0; r->err = 0;
            } else if ((pressed & (B_1 | B_B)) && s == 0) return 1;     /* player 1: back */
            break;
        case PR_NAME: {
            int len = (int)strlen(r->name);
            if (is_kb) { int c; while ((c = plat_text_char())) {        /* (the keyboard can type it) */
                if (c >= 'a' && c <= 'z') c -= 32;
                if (((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) && r->cur < 4) { r->name[r->cur] = (char)c; if (r->cur < 3) r->cur++; }
                else if (c == 8 && r->cur > 0) { r->name[r->cur] = 0; r->cur--; r->name[r->cur] = 0; }
                else if (c == 13) pressed |= B_2;
            } }
            const char *pc = strchr(NAME_CHARS, r->name[r->cur] ? r->name[r->cur] : ' ');
            int ci = pc ? (int)(pc - NAME_CHARS) : 0, nc = (int)sizeof NAME_CHARS - 1;
            if (pressed & NAV_UP) ci = (ci + 1) % nc;
            if (pressed & NAV_DOWN) ci = (ci + nc - 1) % nc;
            if (pressed & (NAV_UP | NAV_DOWN)) {
                r->name[r->cur] = NAME_CHARS[ci] == ' ' ? 0 : NAME_CHARS[ci];
                if (!r->name[r->cur]) for (int k = r->cur + 1; k < 4; k++) r->name[k] = 0;   /* (a blank ends it) */
            }
            if ((pressed & NAV_RIGHT) && r->cur < 3 && r->name[r->cur]) { r->cur++; if (!r->name[r->cur]) r->name[r->cur] = r->name[r->cur - 1] ? 'A' : 0; }
            if ((pressed & NAV_LEFT) && r->cur > 0) r->cur--;
            len = (int)strlen(r->name);
            if (pressed & (B_2 | B_A)) {
                int other = prof_find_name(r->name);
                if (len < 1) r->err = 1;
                else if (other >= 0 && other != r->edit) r->err = 2;    /* (someone has that name) */
                else { r->st = PR_CHAR; r->err = 0; }
            } else if (pressed & (B_1 | B_B)) { r->st = PR_CHOOSE; r->err = 0; }
            break;
        }
        case PR_CHAR: {
            int n = chars_count();
            if (pressed & NAV_LEFT) r->pref = (r->pref + n - 1) % n;
            if (pressed & NAV_RIGHT) r->pref = (r->pref + 1) % n;
            if (pressed & (B_2 | B_A)) {
                int i = r->edit >= 0 ? prof_edit(r->edit, r->name, chars_id(r->pref)) : prof_create(r->name, chars_id(r->pref));
                if (i < 0) { r->st = PR_NAME; r->err = 2; }
                else { r->choice = i; r->st = PR_READY; r->edit = -1; }
            } else if (pressed & (B_1 | B_B)) r->st = PR_NAME;
            break;
        }
        case PR_READY:
            if (pressed & (B_1 | B_B)) r->st = PR_CHOOSE;
            else if ((pressed & (B_2 | B_A | B_PLUS)) && s == 0 && all_ready) {
                int ok = 1; for (int k = 1; k < 8; k++) if (PR[k].active && PR[k].st != PR_READY) ok = 0;
                if (ok) {                                                  /* everyone's in: on to Local / Online */
                    for (int k = 0; k < 8; k++) {
                        int i = PR[k].active && PR[k].choice >= 0 ? PR[k].choice : -1;
                        prof_set_slot(k, i);
                        if (i >= 0) prof_set_controller(k, i);
                        if (PR[k].active) rt_log("profiles", "player slot %d: %s", k + 1, i >= 0 ? prof_name(i) : "guest");
                    }
                    return 2;
                }
            }
            break;
        }
        {   static int last_st[8] = {-1, -1, -1, -1, -1, -1, -1, -1}, last_choice[8];
            if (r->st != last_st[s] || r->choice != last_choice[s]) { last_st[s] = r->st; last_choice[s] = r->choice;
                rt_log("menu", "who's playing: P%d %s %s", s + 1, (const char *[]){"choosing", "name", "character", "ready"}[r->st],
                       r->st == PR_NAME || r->st == PR_CHAR ? r->name : r->choice >= 0 ? prof_name(r->choice) : r->choice == CH_NEW ? "(new)" : "(guest)"); } }
        if (r->st != PR_READY) all_ready = 0;
    }
    (void)any;
    return 0;
}
static void profiles_draw(float a) {
    float fx = rt_widescreen() ? -320.0f / 3 : 0, fw = rt_widescreen() ? 640 + 640.0f / 3 : 640;
    if (!ctl_bg_tried) {
        ctl_bg_tried = 1;
        char p[1200]; int w, h;
        snprintf(p, sizeof p, "%s/ui/%s", g_root, rt_widescreen() ? "controls_bg_169.png" : "controls_bg_43.png");
        uint32_t *px = ui_load_png(p, &w, &h);
        if (px) { ctl_bg = ui_register_texture(px, w, h); free(px); }
    }
    if (ctl_bg) ui_image(ctl_bg, fx, 0, fw, 480, 0, 0, 1, 1, with_alpha(0xFFFFFFFFu, a));
    else ui_rect(fx, 0, fw, 480, with_alpha(0x6EC8F0FFu, a));
    ui_rect(fx, 22, fw, 56, with_alpha(0x10182800u, a * 0.55f));
    gtext_centered(320, 32, 34, "Who's playing?", with_alpha(0xFFFFFFFFu, a));
    const uint32_t WHITE = 0xFFFFFFFFu, GOLD = 0xFFC830FFu, GREEN = 0x80FF80FFu, GREY = 0xA0A8B8FFu, RED = 0xFF8080FFu;
    float x = 30, y = 90, w = 580, h = 340;
    ui_rect(x - 3, y - 3, w + 6, h + 6, with_alpha(0xF0C03000u, a));
    ui_rect(x, y, w, h, with_alpha(0x101828D8u, a * 0.85f));
    double pulse = 0.5 + 0.5 * sin(now_s() * 5.0);
    float ry = y + 12; int all_ready = 1, nrows = 0;
    for (int s = 0; s < 8; s++) {
        prow_t *r = &PR[s];
        if (!r->active) continue;
        nrows++;
        char t[96];
        int kind, conn, pl; uint64_t ad; remotes_slot_info(s, &kind, &ad, &conn, &pl);
        snprintf(t, sizeof t, "P%d", s + 1); gtext(x + 14, ry, 20, t, with_alpha(GOLD, a));
        gtext(x + 56, ry + 3, 15, kind == REMOTES_KEYBOARD || (s == 0 && !conn) ? "Keyboard" : kind == REMOTES_ONLINE ? "Online" : "Wii Remote", with_alpha(GREY, a));
        uint32_t c = WHITE;
        if (r->st == PR_CHOOSE) {
            const char *nm = r->choice >= 0 ? prof_name(r->choice) : r->choice == CH_NEW ? "New profile" : "Guest";
            snprintf(t, sizeof t, "<  %s  >", nm);
            ui_rect(x + 170, ry - 3, 200, 26, with_alpha(0xFFC83000u, a * (0.18f + 0.12f * (float)pulse)));
            gtext(x + 180, ry, 20, t, with_alpha(WHITE, a));
            if (r->choice >= 0) { const char *pc = chars_name(chars_index(prof_pref(r->choice))); gtext(x + 390, ry + 3, 15, pc[0] ? pc : "-", with_alpha(GREY, a)); }
            all_ready = 0;
        } else if (r->st == PR_NAME) {
            char nm[5] = "    "; for (int k = 0; k < 4 && r->name[k]; k++) nm[k] = r->name[k];
            for (int k = 0; k < 4; k++) {
                char ch[2] = {nm[k] == ' ' ? '_' : nm[k], 0};
                float cx = x + 190 + k * 26;
                if (k == r->cur) ui_rect(cx - 4, ry - 3, 24, 26, with_alpha(0xFFC83000u, a * (0.25f + 0.2f * (float)pulse)));
                gtext(cx, ry, 22, ch, with_alpha(k == r->cur ? GOLD : WHITE, a));
            }
            gtext(x + 300, ry + 3, 14, r->err == 1 ? "type a name" : r->err == 2 ? "that name is taken" : "username", with_alpha(r->err ? RED : GREY, a));
            all_ready = 0;
        } else if (r->st == PR_CHAR) {
            snprintf(t, sizeof t, "%s:  <  %s  >", r->name, chars_name(r->pref));
            gtext(x + 180, ry, 19, t, with_alpha(WHITE, a));
            gtext(x + 180, ry + 22, 13, "preferred character", with_alpha(GREY, a));
            all_ready = 0;
        } else {
            const char *nm = r->choice >= 0 ? prof_name(r->choice) : "Guest";
            gtext(x + 180, ry, 20, nm, with_alpha(GREEN, a));
            if (r->choice >= 0) { const char *pc = chars_name(chars_index(prof_pref(r->choice))); gtext(x + 260, ry + 3, 15, pc, with_alpha(GREY, a)); }
            gtext(x + w - 90, ry + 2, 18, "Ready", with_alpha(GREEN, a));
        }
        (void)c;
        ry += r->st == PR_CHAR ? 46 : 36;
    }
    if (!nrows) gtext(x + 20, ry, 18, "No controllers connected", with_alpha(GREY, a));
    gtext_centered(320, y + h - 46, 14, "Each controller: Left/Right choose, 2 take it, + edit a profile. New profile: a 4-letter name (Up/Down, or type), then a character.",
                   with_alpha(GREY, a));
    gtext_centered(320, 456, 20, all_ready && nrows ? "Player 1 - 2: Continue   1: Back" : "1: Back", with_alpha(WHITE, a));
}
void menu_controls_open(void) { ctl_open = 1; ctl_t = now_s(); rt_log("menu", "controls (from the character select)"); }
int menu_controls_is_open(void) { return ctl_open; }
/* host, character select, before the online game: who's in, and the start */
static void online_start_draw(void) {
    const uint32_t WHITE = 0xFFFFFFFFu, GOLD = 0xFFC830FFu, GREY = 0xA0A8B8FFu;
    float x = 50, y = 372, w = 540, h = 92;
    ui_rect(x - 3, y - 3, w + 6, h + 6, 0xF0C030E0u);
    ui_rect(x, y, w, h, 0x101828E8u);
    gtext(x + 16, y + 8, 22, "Online game", GOLD);
    char names[200] = ""; char nm[48]; int slot, ping, n = 0;
    for (int i = 0; i < 8 && net_lobby(i, nm, sizeof nm, &slot, &ping); i++, n++) {
        if (strlen(names) + strlen(nm) + 3 < sizeof names) { if (n) strcat(names, ", "); strcat(names, nm); }
    }
    gtext(x + 16, y + 36, 16, names, WHITE);
    const char *hint = net_online_starting() ? net_status() : net_online_players() > 0 ? "2: Start the online game     1: Back" : "Waiting for players to join...";
    gtext(x + 16, y + 62, 18, hint, net_online_starting() ? GREY : WHITE);
}
/* ================================================================== the update screen
 * After "hold the Wii Remote sideways" the boot waits here (rt_update.c holds the title back).
 * It is that screen's own layout again (rt_bootscreen.c): the game's fader fades it back in, its
 * picture box and "2" button are hidden, and its text box says what is going on, in the game's
 * font. Drawn here, over it: a spinner while it checks, then a download bar; the version; "1: Skip".
 * The result stays up a moment; 1/B skips a download. Times are wall-clock.
 * Where things go: the layout is 608 x 456 units round the screen's middle, y up; this overlay
 * is 640 x 480 - the same scale (480 / 456) both ways, so a layout point (x, y) is
 * (320 + x * k, 240 - y * k) here. */
#define LK (480.0f / 456.0f)
static float lx(float x) { return 320 + x * LK; }
static float ly(float y) { return 240 - y * LK; }
static char upd_main[200];                                 /* the text box's line(s) (rt_bootscreen.c) */
const char *menu_update_text(void) { return upd_main; }
static void update_spinner(float cx, float cy, float a) {
    double ph = now_wall() * 1.4; int n = 12;
    for (int i = 0; i < n; i++) {
        double ang = (double)i / n * 6.2831853, d = fmod(ph - (double)i / n + 2.0, 1.0);   /* (0 = the leading dot) */
        float x = cx + 22.0f * (float)cos(ang), y = cy + 22.0f * (float)sin(ang), k = (float)(1.0 - d) * 0.85f + 0.15f;
        ui_rect(x - 3.5f, y - 3.5f, 7, 7, with_alpha(0x46568C00u, a * k));
    }
}
static void update_text_fit(float y, float size, const char *t, uint32_t rgba) {
    while (size > 10 && gtext_width(t, size) > 480) size -= 1;
    gtext_centered(320, y, size, t, rgba);
}
static void update_screen(void) {
    struct { double t0, t_end; } us; update_screen_times(&us.t0, &us.t_end);
    if (!us.t0) return;                                    /* (never shown) */
    double now = now_wall();
    char msg[256], ver[32], line[128]; int64_t got, total; double bps;
    int s = update_status(msg, sizeof msg, &got, &total, &bps, ver, sizeof ver);
    if (update_screen_active()) font_load_boot();
    static double fd = -1; if (fd < 0) { const char *e = getenv("RT_UPDATE_FADE"); fd = e ? atof(e) : 0.3; if (fd <= 0) fd = 0.3; }
    float a = (float)((now - us.t0 - fd / 2) / (fd / 2)); if (a < 0) a = 0; if (a > 1) a = 1;   /* (in: with the layout's new text, rt_bootscreen.c) */
    float dark = 0;                                        /* out: to black, as the game goes to the title */
    if (us.t_end) { double k = (now - us.t_end) / 0.45; if (k > 1.8) return; if (k >= 1) { ui_rect(0, 0, 640, 480, 0x000000FFu); return; } dark = (float)k; a = 1; }
    const uint32_t SOFT = 0xF0F4FF00u, BLUE = 0x46568C00u;   /* (the font has its own dark edge, like the layout's text) */
    float cy = ly(62);                                     /* above the text (rt_bootscreen.c UPD_TEXT_Y), where the picture box was */
    snprintf(line, sizeof line, "Version %s", rt_version());
    gtext(lx(236) - gtext_width(line, 14), ly(-150), 14, line, with_alpha(SOFT, a));   /* (where the "2" button was) */
    int checking = s == UPD_IDLE || s == UPD_CHECKING || ((s == UPD_LATEST || s == UPD_NO_SERVER) && now - us.t0 < 0.7);
    if (checking) {
        snprintf(upd_main, sizeof upd_main, "Checking for updates");
        update_spinner(320, cy, a);
    } else if (s == UPD_DOWNLOADING || s == UPD_INSTALLING || s == UPD_RESTART) {
        snprintf(upd_main, sizeof upd_main, "%s", msg);
        float w = 280, h = 20, x = 320 - w / 2, y = cy - 18, f = total > 0 ? (float)((double)got / (double)total) : 0;
        if (s != UPD_DOWNLOADING) f = 1;
        if (f > 1) f = 1;
        ui_rect(x - 3, y - 3, w + 6, h + 6, with_alpha(0x7E8CB000u, a));
        ui_rect(x, y, w, h, with_alpha(0xFFFFFF00u, a));
        ui_rect(x, y, w * f, h, with_alpha(0xF0B42800u, a));
        if (f > 0.01f) ui_rect(x, y + 2, w * f, h * 0.3f, with_alpha(0xFFE49000u, a * 0.7f));   /* (shine) */
        if (s == UPD_DOWNLOADING) {
            char sp[32] = "";
            if (bps >= 1e6) snprintf(sp, sizeof sp, "  -  %.1f MB/s", bps / 1e6); else if (bps > 1) snprintf(sp, sizeof sp, "  -  %.0f KB/s", bps / 1e3);
            snprintf(line, sizeof line, "%.1f of %.1f MB%s", got / 1e6, total / 1e6, sp);
            gtext_centered(320, y + h + 10, 18, line, with_alpha(SOFT, a));
            gtext(lx(-236), ly(-150), 14, "1: Skip (download it next time)", with_alpha(SOFT, a));
        }
    } else {
        const char *t = s == UPD_SKIPPED ? "Update skipped" : msg;
        const char *colon = s == UPD_FAILED ? strstr(t, ": ") : NULL;   /* "Update failed: why": the why under the box's middle */
        if (colon) {
            size_t l = (size_t)(colon - t); if (l > sizeof upd_main - 1) l = sizeof upd_main - 1;
            memcpy(upd_main, t, l); upd_main[l] = 0;
            update_text_fit(cy - 10, 18, colon + 2, with_alpha(SOFT, a));
        } else snprintf(upd_main, sizeof upd_main, "%s", t);
        if (s == UPD_LATEST) update_text_fit(cy - 10, 20, line, with_alpha(SOFT, a));
        (void)BLUE;
    }
    if (dark > 0) ui_rect(0, 0, 640, 480, with_alpha(0x00000000u, dark));
}
void menu_draw(void) {
    void discord_frame(void); discord_frame();       /* rt_discord.c: Discord rich presence */
    update_screen();
    if (update_screen_active()) return;
    if (online_overlay()) online_start_draw();
    {   const char *nt = net_notice();                  /* an online game just ended: why */
        if (nt) { float w = gtext_width(nt, 18) + 40; if (w > 600) w = 600; float x = 320 - w / 2, y = 24;
                  ui_rect(x - 3, y - 3, w + 6, 40, 0xF0C030E0u); ui_rect(x, y, w, 34, 0x101828F0u); gtext_centered(320, y + 7, 18, nt, 0xFFFFFFFFu); } }
    if (ctl_open) { double t = (now_s() - ctl_t) / FADE; controls_draw(t > 1 ? 1 : (float)t); return; }
    if (!title_live() && tstate != T_OPTIONS && tstate != T_CONTROLS && tstate != T_ONLINE && tstate != T_PROFILES) return;
    if (tstate == T_PROFILES) { profiles_draw(fade_t()); return; }
    if (tstate == T_CONTROLS) { controls_draw(fade_t()); return; }
    if (tstate == T_ONLINE) { online_draw(fade_t()); return; }
    float t = fade_t();
    if (tstate == T_LIST || tstate == T_LIST_OUT) {
        float a = tstate == T_LIST ? t : 1 - t;
        float size = press_size * 0.92f, gap = size * 1.18f, y0 = press_y - gap * (LIST_N - 1) / 2 - size / 2;
        double pulse = 0.5 + 0.5 * sin(now_s() * 5.0);
        for (int i = 0; i < LIST_N; i++) {
            int on = i == sel;
            uint32_t c = on ? 0xFFFFFF00u : 0xB4B4BC00u;
            float s = on ? size * (1.0f + 0.03f * (float)pulse) : size * 0.92f;
            gtext_centered(press_x, y0 + i * gap + (size - s) / 2, s, list_item(i), with_alpha(c, a));
        }
    } else if (tstate == T_OPTIONS) {
        /* below the title logo */
        float a = t;
        float x = 70, y = 200, w = 500, h = 262;
        ui_rect(x - 3, y - 3, w + 6, h + 6, with_alpha(0xF0C03000u, a));
        ui_rect(x, y, w, h, with_alpha(0x101828D8u, a * 0.85f));
        float row = 27, ty = y + 12, size = 21;
        for (int i = 0; i < O_ITEMS; i++) {
            char label[64], value[64]; option_text(i, label, value);
            int on = i == opt_sel;
            if (on) ui_rect(x + 10, ty - 4, w - 20, row - 2, with_alpha(0xE0402000u, a * 0.9f));
            uint32_t c = with_alpha(0xFFFFFFFFu, a);
            gtext(x + 24, ty, size, label, c);
            char v[80]; snprintf(v, sizeof v, on && i != O_CONTROLS ? "<  %s  >" : "%s", i == O_CONTROLS ? ">" : value);
            gtext(x + w - 24 - gtext_width(v, size), ty, size, v, c);
            ty += row;
        }
        /* bottom row: Cancel (left), Save / Save & Restart (right) */
        float by = y + h - 46, bw = 200, bh = 34;
        const char *save = needs_restart() ? "Save & Restart" : "Save";
        uint32_t gold = 0xFFC830FFu;
        if (opt_sel == O_CANCEL) ui_rect(x + 14, by - 4, bw, bh, with_alpha(0xE0402000u, a * 0.9f));
        if (opt_sel == O_SAVE) ui_rect(x + w - 14 - bw, by - 4, bw, bh, with_alpha(needs_restart() ? 0xA07010FFu : 0xE04020FFu, a * 0.9f));
        { char vs[40]; snprintf(vs, sizeof vs, "Version %s", rt_version()); gtext(x + w - gtext_width(vs, 14), y + h + 7, 14, vs, with_alpha(0xC8D0E0FFu, a)); }
        gtext_centered(x + 14 + bw / 2, by, 26, "Cancel", with_alpha(0xFFFFFFFFu, a));
        gtext_centered(x + w - 14 - bw / 2, by, needs_restart() ? 23 : 26, save, with_alpha(needs_restart() ? gold : 0xFFFFFFFFu, a));
    }
}
