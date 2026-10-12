/* rt_menu.c - menus added to the game's title screen.
 *
 * Title screen: "Press 2 to Start" works as usual until 2 is pressed; then "Press 2 to Start" and
 * the copyright line fade out and a list fades in: Singleplayer / Multiplayer / Options, drawn
 * with the game's own font (taken from the "Press 2" text box). While the list is up the title
 * doesn't time out into its demo movie.
 *   Singleplayer - continues like the original 2 press, into a file select without the
 *                  Free-for-All / Coin Battle buttons
 *   Multiplayer  - (character select comes next; for now the original file select)
 *   Options      - display, window, resolution, aspect ratio, HD textures, real Wii Remote;
 *                  saved to settings.ini; "Restart" appears when a change needs one
 * Input: the remote held sideways (d-pad moves, 2/A chooses, 1/B goes back). While our menus are
 * up the game sees no buttons. Drawn as an overlay (menu_draw) on a 640x480 virtual screen. */
#include "rt.h"
#include <math.h>
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
static double now_s(void) { return rt_now_ns() / 1e9; }

/* ================================================================== the game's font
 * nw4r::ut::ResFont (pointer at TextBox+0xE4): +0x14 = FINF (font info, pointers relocated). */
static struct {
    int ok, tex;
    int cell_w, cell_h, row, line, sheet_w, sheet_h, nsheets, height;
    int def_left, def_glyph, def_char;
    uint32_t widths, cmap;
    float atlas_w, atlas_h; int per_row;
} gf;
static int valid_ptr(uint32_t p) { return (p >= 0x80000000u && p < 0x81900000u) || (p >= 0x90000000u && p < 0x94000000u); }
static void font_load(uint32_t textbox) {
    static int tries; if (tries++ > 3) return;
    uint32_t font = R32(textbox + 0xE4), finf = valid_ptr(font) ? R32(font + 0x14) : 0, tglp = valid_ptr(finf) ? R32(finf + 8) : 0;
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
enum { T_IDLE, T_PRESS_OUT, T_LIST, T_LIST_OUT, T_PRESS_IN, T_OPTIONS, T_START, T_CONTROLS };
enum { NEXT_NONE, NEXT_SINGLE, NEXT_MULTI, NEXT_OPTIONS, NEXT_TITLE };
#define FADE 0.25
static int tstate = T_IDLE, next_action, sel, opt_sel;
static double t_state;
static uint32_t prev_btn;
static uint64_t inject_until, inject_t0;           /* replaying a 2 press to the game */
static int singleplayer_mode;                      /* file select without the multiplayer buttons */
/* title layout */
static uint32_t press_pane, copy_pane; static uint64_t title_seen_frame; static int press_ready;
static float press_x = 320, press_y = 380, press_size = 35;
/* file select layout */
static uint64_t fs_seen_frame; static int fs_visible, fs_was_visible; static uint64_t fs_left_frame; static int auto_one_done;

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
/* singleplayer file select: the file cards and the "Select a File" title move down 70 (no
   buttons under them any more) */
static const char *fs_shift_names[2] = {"N_fileSelect_00", "N_titlePos_00"};
static uint32_t fs_shift_pane[2]; static float fs_shift_base[2], fs_shift_written[2];
void menu_layout_calc_hook(uint32_t layout, uint32_t root) {
    (void)layout;
    if (layout_kind(root) != 2) return;
    for (int i = 0; i < 2; i++) {
        uint32_t p = lyt_find_pane(root, fs_shift_names[i]);
        if (!p) continue;
        float y = rdf(p + 0x30);                         /* mTranslate.y */
        if (p != fs_shift_pane[i] || y != fs_shift_written[i]) { fs_shift_pane[i] = p; fs_shift_base[i] = y; }   /* (re)set by the game */
        fs_shift_written[i] = singleplayer_mode ? fs_shift_base[i] - 70.0f : fs_shift_base[i];
        wrf(p + 0x30, fs_shift_written[i]);
    }
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
void hle_fn_80055FF0(ppc_context *ctx, uint8_t *base) { if (cam_held()) return; orig_fn_80055FF0(ctx, base); }
void hle_fn_80055D00(ppc_context *ctx, uint8_t *base) { if (cam_held()) return; orig_fn_80055D00(ctx, base); }   /* (screen edge push) */
void hle_fn_80055810(ppc_context *ctx, uint8_t *base) { if (cam_held()) return; orig_fn_80055810(ctx, base); }   /* (scroll limits) */
/* the title players' walk is fixed by the game each frame; "running" adds the difference to
   their position (their walk animation stays) */
static void set_speed(int i, float target) {
    if (!title_players[i] || target <= WALK) return;
    uint32_t p = title_players[i]; float d = target - WALK;
    wrf(p + 0xAC, rdf(p + 0xAC) + d); wrf(p + 0xB8, rdf(p + 0xB8) + d);
}
/* dGameKey_c::read: the title players read their (demo) remotes; holding 1 makes them run */
static int run_key; static int runin_done[8];
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
    cam_mode = CAM_OUTRUN;                                 /* (reopened while running in: they run off again) */
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
static void title_demo_update(void) {
    uint32_t cnt = R32(G_STAGE_COUNT);
    if (cnt < cam_last_count) { cam_mode = CAM_FOLLOW; cam_paused = 0; }   /* a new title run */
    cam_last_count = cnt;
    float w = screen_w();
    if (cam_mode == CAM_OUTRUN) {                          /* run until past the right edge */
        int off = 1;
        for (int i = 0; i < 8; i++) if (title_players[i]) { set_speed(i, RUN); if (rdf(title_players[i] + 0xAC) < cam_x + w + 24) off = 0; }
        if (off) { cam_mode = CAM_PARKED; rt_log("tdemo", "players off screen"); }
    }
    if (cam_mode == CAM_PARKED)                            /* kept just past the right edge, out of view */
        for (int i = 0; i < 8; i++) if (title_players[i]) {
            float x = cam_x + w + 40 + i * 24;
            wrf(title_players[i] + 0xAC, x); wrf(title_players[i] + 0xB8, x);
            wrf(title_players[i] + P_SPEEDF, WALK); set_speed(i, WALK);
        }
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
/* ---- singleplayer: no "Select Players" step ----
   After the file select, the game shows "Select Players" (the layout is hidden in singleplayer).
   Its wait-for-input state (fn_2_3F000) is answered at once with 1 player, its layout
   animations finish instantly, and its sounds are muted; the file pick itself plays the
   solo start sound (SE 803) instead of the "decide" click. */
static int sp_skip(void) { return singleplayer_mode && R16(G_NOW_SCENE) == SCENE_GAME_SETUP; }
#define SP_CODE_LO 0x807ABD00u                     /* the Select Players actor's code (REL) */
#define SP_CODE_HI 0x807ACD00u
uint32_t mod_game_addr(uint32_t a);
static int sp_code(uint32_t lr) { lr = mod_game_addr(lr); return lr >= SP_CODE_LO && lr < SP_CODE_HI; }
void hle_fn_2_3F000(ppc_context *ctx, uint8_t *base) {
    if (!sp_skip()) { orig_fn_2_3F000(ctx, base); return; }
    uint32_t self = ctx->r[3], sm = self + 112;
    W32(self + 620, 0);                                 /* cursor: 1 player */
    W32(self + 624, 1);                                 /* decided */
    ctx->r[3] = sm; ctx->r[4] = 0x809953B8u + 400;      /* -> the "decided" state */
    ppc_call_indirect(ctx, base, R32(R32(sm) + 24));
    rt_log("menu", "singleplayer: 1 player");
}
void hle_isAnimOn__Q23m2d13EmbedLayout_cFi(ppc_context *ctx, uint8_t *base) {
    if (sp_skip() && sp_code(ctx->lr)) { ctx->r[3] = 0; return; }
    orig_isAnimOn__Q23m2d13EmbedLayout_cFi(ctx, base);
}
/* game setup music (fn_8019BA20(mgr, track)): track 0 plays on the file select, track 1 for
   "Select Players"; singleplayer stays on the file select's track */
void hle_fn_8019BA20(ppc_context *ctx, uint8_t *base) {
    if (sp_skip() && ctx->r[4] == 1 && ctx->lr == 0x8091999Cu) return;
    orig_fn_8019BA20(ctx, base);
}
/* system sound filter (SndAudioMgr::startSystemSe): returns the id to play, 0 = none */
uint32_t menu_filter_se(uint32_t id, uint32_t lr) {
    if (!sp_skip()) return id;
    if (sp_code(lr)) return 0;              /* the hidden screen */
    if (id == 121 && lr == 0x8078405Cu) return 803;                  /* file picked: start sound */
    return id;
}
static int select_players_hidden;
void menu_layout_hook(uint32_t layout, uint32_t root) {
    (void)layout;
    int kind = layout_kind(root);
    { static int on = -1; if (on < 0) on = getenv("RT_PANETRACE") != NULL;
      if (on && g_frames_presented % 60 == 0) {
          static const char *nm[] = {"N_guideViewL_00", "N_left_00", "N_left_01", "T_world_00", "N_worldBase_00", "N_proportionR_00"};
          char b[600]; int n = 0;
          for (int i = 0; i < 6; i++) { uint32_t p = lyt_find_pane(root, nm[i]); if (p) n += snprintf(b + n, sizeof b - n, " %s a%u/%u f%02X y%.0f gy%.0f", nm[i], R8(p + 0xB8), R8(p + 0xB9), R8(p + 0xBB), rdf(p + 0x30), rdf(p + 0xA0)); }
          if (n) rt_log("pane", "f%llu%s", (unsigned long long)g_frames_presented, b); } }
    if (kind == 4) {                                     /* singleplayer: "Select Players" is answered for you, unseen */
        if (singleplayer_mode && R16(G_NOW_SCENE) == SCENE_GAME_SETUP) { lyt_set_visible(root, 0); select_players_hidden = 1; }
        else if (select_players_hidden) { lyt_set_visible(root, 1); select_players_hidden = 0; }
        return;
    }
    if (kind == 1) {
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
    } else if (kind == 2) {
        fs_seen_frame = g_frames_presented;
        uint32_t fsel = lyt_find_pane(root, "N_fileSelect_00");
        fs_visible = fsel && (R8(fsel + 0xBB) & 1) && lyt_get_alpha(fsel) > 0;
        if (singleplayer_mode) {                       /* no Free-for-All / Coin Battle */
            lyt_set_visible(lyt_find_pane(root, "N_Multi_00"), 0);
            lyt_set_visible(lyt_find_pane(root, "N_MultiCoin_00"), 0);
        }
    }
}

/* the title's own timeout into its demo movie is held while our menus are up
   (fn_2_14800 = the title's check of dScStage frame count > 900 -> opening movie / replay) */
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
static const char *list_items[3] = {"Singleplayer", "Multiplayer", "Options"};

static void start_game(int single) {
    rt_log("menu", "%s: continue into the game", single ? "singleplayer" : "multiplayer");
    singleplayer_mode = single;
    set_state(T_IDLE);
    inject_t0 = rt_now_ns(); inject_until = inject_t0 + 4000000000ull;
}

uint32_t menu_filter_buttons(uint32_t b) {
    uint32_t pressed = b & ~prev_btn; prev_btn = b;
    uint64_t now = rt_now_ns();
    { static int tr = -1; if (tr < 0) tr = getenv("RT_MENUTRACE") != NULL;
      if (tr && g_frames_presented % 60 == 0) rt_log("menutrace", "scene %u mode %u title_seen %llu (frame %llu) press_ready %d tstate %d",
          R16(G_NOW_SCENE), R32(G_GAME_MODE), (unsigned long long)title_seen_frame, (unsigned long long)g_frames_presented, press_ready, (int)tstate); }
    int scene = R16(G_NOW_SCENE);
    if (scene == SCENE_WORLD_MAP || (title_live() && !inject_until)) { singleplayer_mode = 0; fs_left_frame = 0; }   /* reached the map, or back at the title */
    if (inject_until) {                                     /* replay "2" until the game leaves the title */
        if (now > inject_until || !at_title()) { inject_until = 0; title_seen_frame = 0; return 0; }
        return ((now - inject_t0) / 1000000ull) % 250 < 100 ? B_2 : 0;
    }
    /* singleplayer: the "Select Players" screen that follows the file select starts on 1 player;
       confirm it automatically */
    int fs_active = scene == SCENE_GAME_SETUP && fs_visible && g_frames_presented - fs_seen_frame <= 3;
    if (fs_was_visible && !fs_active && scene == SCENE_GAME_SETUP) { fs_left_frame = g_frames_presented; auto_one_done = 0; }
    if (!fs_was_visible && fs_active) auto_one_done = 1;      /* back on the file select (e.g. Back from the next screen) */
    fs_was_visible = fs_active;
    if (singleplayer_mode && scene == SCENE_GAME_SETUP && !fs_active && fs_left_frame && !auto_one_done) {
        /* "Select Players" (hidden) is answered with 1 player by hle_fn_2_3F000; no input
           reaches it meanwhile (gives up after 600 frames) */
        if (g_frames_presented - fs_left_frame > 600) { auto_one_done = 1; return b; }
        return b & ~0xFFFFu;
    }
    /* singleplayer file select: Down would lead to the (hidden) multiplayer buttons */
    if (singleplayer_mode && fs_active) b &= ~NAV_DOWN;
    if (!title_live()) {                                    /* left the title (demo, reset...): drop our menus */
        if (tstate != T_IDLE) set_state(T_IDLE);
        cam_mode = CAM_FOLLOW;
        return b;
    }
    switch (tstate) {
    case T_IDLE:
        if ((pressed & B_2) && press_ready) { set_state(T_PRESS_OUT); next_action = NEXT_NONE; sel = 0; cam_freeze(); return 0; }
        return b;
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
            else set_state(T_PRESS_IN);
        }
        return 0;
    case T_LIST:
        if (fade_t() < 0.6f) return 0;
        if (pressed & NAV_UP) sel = (sel + 2) % 3;
        if (pressed & NAV_DOWN) sel = (sel + 1) % 3;
        if (pressed & (B_2 | B_A)) { next_action = sel == 0 ? NEXT_SINGLE : sel == 1 ? NEXT_MULTI : NEXT_OPTIONS; set_state(T_LIST_OUT); }
        else if (pressed & (B_1 | B_B)) { next_action = NEXT_TITLE; set_state(T_LIST_OUT); cam_return(); }
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
                set_state(T_LIST); sel = 2;
                return 0;
            }
        }
        if (pressed & (B_1 | B_B)) { options_cancel(); set_state(T_LIST); sel = 2; }
        return 0;
    case T_CONTROLS:                                        /* (placeholder screen for now) */
        if (fade_t() < 0.6f) return 0;
        if (pressed & (B_1 | B_B | B_2 | B_A)) { set_state(T_OPTIONS); t_state -= FADE; }
        return 0;
    }
    return 0;
}

/* ================================================================== drawing */
static uint32_t with_alpha(uint32_t rgb, float a) { int v = (int)(a * 255); if (v < 0) v = 0; if (v > 255) v = 255; return (rgb & 0xFFFFFF00u) | (uint32_t)v; }
/* ---- Controls screen: the file select's background, the keyboard layout and the remote ----
   (a first version: keybinds and remote pairing get edited here later) */
static int ctl_bg; static int ctl_bg_tried;
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
    ui_rect(fx, 34, fw, 64, with_alpha(0x10182800u, a * 0.55f));          /* title band */
    gtext_centered(320, 50, 36, "Controls", with_alpha(0xFFFFFFFFu, a));
    float x = 60, y = 128, w = 520, h = 300;
    ui_rect(x - 3, y - 3, w + 6, h + 6, with_alpha(0xF0C03000u, a));
    ui_rect(x, y, w, h, with_alpha(0x101828D8u, a * 0.85f));
    static const char *keys[][2] = {{"Move", "Arrow keys"}, {"Jump (2)", "X"}, {"Run / Throw (1)", "Z"}, {"Shake", "C"},
                                    {"A / B", "A / S"}, {"+ / -", "Enter / Shift"}, {"Home", "H"}};
    float ty = y + 14, size = 21;
    gtext(x + 20, ty, 22, "Keyboard", with_alpha(0xFFC830FFu, a)); ty += 30;
    for (int i = 0; i < 7; i++) {
        gtext(x + 34, ty, size, keys[i][0], with_alpha(0xFFFFFFFFu, a));
        gtext(x + w - 24 - gtext_width(keys[i][1], size), ty, size, keys[i][1], with_alpha(0xFFFFFFFFu, a));
        ty += 26;
    }
    ty += 8;
    gtext(x + 20, ty, 22, "Wii Remote", with_alpha(0xFFC830FFu, a));
    int st = wm_status();
    const char *rs = st == 2 ? "Connected" : "Not connected (keyboard)";
    gtext(x + w - 24 - gtext_width(rs, size), ty, size, rs, with_alpha(st == 2 ? 0x80FF80FFu : 0xFFFFFFFFu, a));
    gtext_centered(320, 444, 22, "1: Back", with_alpha(0xFFFFFFFFu, a));
}
void menu_draw(void) {
    /* singleplayer: from the file select's fade-out until the world map, the screen stays black
       (the hidden "Select Players" step would otherwise fade the empty background in and out) */
    static int noblack = -1; if (noblack < 0) noblack = getenv("RT_NOSPBLACK") != NULL;
    if (!noblack && sp_skip() && fs_left_frame && !auto_one_done && g_frames_presented - fs_seen_frame > 3) { ui_rect(-240, -10, 1120, 500, 0x000000FFu); return; }
    if (!title_live() && tstate != T_OPTIONS && tstate != T_CONTROLS) return;
    if (tstate == T_CONTROLS) { controls_draw(fade_t()); return; }
    float t = fade_t();
    if (tstate == T_LIST || tstate == T_LIST_OUT) {
        float a = tstate == T_LIST ? t : 1 - t;
        float size = press_size, gap = size * 1.25f, y0 = press_y - gap - size / 2;
        double pulse = 0.5 + 0.5 * sin(now_s() * 5.0);
        for (int i = 0; i < 3; i++) {
            int on = i == sel;
            uint32_t c = on ? 0xFFFFFF00u : 0xB4B4BC00u;
            float s = on ? size * (1.0f + 0.03f * (float)pulse) : size * 0.92f;
            gtext_centered(press_x, y0 + i * gap + (size - s) / 2, s, list_items[i], with_alpha(c, a));
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
        gtext_centered(x + 14 + bw / 2, by, 26, "Cancel", with_alpha(0xFFFFFFFFu, a));
        gtext_centered(x + w - 14 - bw / 2, by, needs_restart() ? 23 : 26, save, with_alpha(needs_restart() ? gold : 0xFFFFFFFFu, a));
    }
}
