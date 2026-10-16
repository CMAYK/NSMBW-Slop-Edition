/* rt_bootscreen.c - the boot's "Hold the Wii Remote sideways." screen, for a keyboard player.
 *
 * After the safety screen the boot (dScBoot_c, ControllerInformation states) shows a picture of
 * two hands holding the Wii Remote sideways, with "Hold the Wii Remote sideways." under it. When
 * player 1 plays on the keyboard, 2 on it doesn't go on to the title yet: the same screen then
 * shows two hands holding a keyboard sideways with a red "no" sign over it, and "Do not hold your
 * keyboard sideways." in the game's own font; 2 again goes on.
 *
 * The layout (dump: RT_LYTDUMP=1):
 *   N_contInfo_00
 *     W_contInfo_00                  the striped panel
 *     T_contInfo_00 / T_contInfo_01  the text: its shadow, then the text (font 32)
 *     W_yokomochi_00 (300 x 180)     the picture box: white inside, green frame 4 units wide
 *       P_yoko_00 (384 x 224, scale 0.7) the picture: an RGB5A3 384x220 texture (drawn after the box, so over its frame)
 *     N_btn_00                       the "2" button
 * The picture is ui/graphics/boot/keyboard_sideways.png. Its width fills the inside of the box
 * (292 units). Its height follows from its shape: the drawing is the middle 172 units (the box's
 * inside), and what is above and below that (transparent but for the sign) shows over the
 * box's frame. The file is turned into a GX RGB5A3 texture (16 bits, with 3-bit alpha) in our guest
 * memory; it borrows the game picture region (rt_gamesetup.c), which is free until Game Select. */
#include "rt.h"
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

uint32_t gs_ui_mem_borrow(uint32_t bytes);      /* rt_gamesetup.c */

#define P_SIZE      0x4C                        /* nw4r::lyt::Pane: mSize (w, h) */
#define P_TRANS     0x2C                        /* mTranslate (x, y) */
#define P_SCALE     0x44                        /* mScale (x, y) */
#define P_NAME      0xBC
#define P_CHILDREN  0x10
#define PIC_VTX     0xD8                        /* nw4r::lyt::Picture: ut::Color[4] */
#define PANE_MAT    0x28
#define MAT_TEXMAP  0x40                        /* the first TexMap: image, palette, u16 w, h, ..., u32 bits (format: top 4) */
#define TB_TEXT     0xD8                        /* nw4r::lyt::TextBox: text buffer, ..., u16 capacity (bytes) at +0xFC, length at +0xFE */
#define UPD_TEXT_Y  (-40.0f)                    /* the update screen's text, in layout units (the layout's own: -87) */
#define BOX_INSIDE  292.0f                      /* W_yokomochi_00's inside: 300 - 2 x 4 */

static const char TEXT[] = "Do not hold your\nkeyboard sideways.";

static int tex_state;                           /* 0 untried, 1 ok, -1 none */
static uint32_t tex_addr; static int tex_w, tex_h;
static float upd_k(void);
void boot_screen_mem_lost(void) { if (tex_state > 0) tex_state = 0; }   /* (Game Select took the memory back) */

static float rdf(uint32_t a) { uint32_t u = R32(a); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); W32(a, u); }

/* is this the controller information layout? (a direct child of the root named N_contInfo_00) */
static uint32_t cont_info(uint32_t root) {
    uint32_t sentinel = root + P_CHILDREN + 4, node = R32(sentinel);
    for (int guard = 0; node && node != sentinel && guard < 64; guard++) {
        uint32_t p = node - 4; char n[18]; for (int i = 0; i < 17; i++) n[i] = (char)R8(p + P_NAME + i); n[17] = 0;
        if (!strcmp(n, "N_contInfo_00")) return p;
        node = R32(node);
    }
    return 0;
}
/* player 1 on the keyboard? (RT_BOOT_KEYBOARD=0/1 decides, for testing) */
static int keyboard_player(void) {
    static int force = -2; if (force == -2) { const char *e = getenv("RT_BOOT_KEYBOARD"); force = e ? atoi(e) : -1; }
    if (force >= 0) return force;
    return remotes_slot_keyboard(0);
}
static int load_picture(void) {
    if (tex_state) return tex_state > 0;
    tex_state = -1;
    char path[1200]; snprintf(path, sizeof path, "%s/ui/graphics/boot/keyboard_sideways.png", g_root);
    int w, h; uint32_t *px = ui_load_png(path, &w, &h);
    if (!px) { rt_log("boot", "%s: not there - the console's picture stays", path); return 0; }
    int tw = (w + 3) & ~3, th = (h + 3) & ~3;
    if (w > 1024 || h > 1024) { free(px); rt_log("boot", "%s: too big (1024 x 1024 at most)", path); return 0; }
    uint32_t bytes = (uint32_t)tw * th * 2, at = gs_ui_mem_borrow(bytes);
    if (!at) { free(px); rt_log("boot", "%s: too big for the picture memory", path); return 0; }
    uint8_t *dst = G(at);                       /* GX RGB5A3: 4x4 tiles, 16-bit big-endian texels */
    for (int ty = 0; ty < th; ty += 4) for (int tx = 0; tx < tw; tx += 4)
        for (int i = 0; i < 16; i++) {
            int x = tx + (i & 3), y = ty + (i >> 2);
            uint32_t c = (x < w && y < h) ? px[y * w + x] : 0;
            uint32_t a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255; uint16_t v;
            if (a >= 0xE0) v = (uint16_t)(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
            else v = (uint16_t)((a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
            *dst++ = (uint8_t)(v >> 8); *dst++ = (uint8_t)v;
        }
    free(px);
    tex_addr = at; tex_w = w; tex_h = h; tex_state = 1;
    rt_log("boot", "keyboard picture: %dx%d (%u KB)", w, h, bytes / 1024);
    return 1;
}

/* ---- the order: the console's "Hold the Wii Remote sideways." first, as it is. When it is done
 * (2 pressed: the boot asks for the title), the boot waits here: the picture and text change over
 * 0.3 s to the keyboard's, and 2 (or A) goes on - to the update screen or the title. Only for a
 * keyboard player 1, and not in deterministic test runs (RT_BOOT_KEYBOARD=1 forces it). */
static double kb_t0, kb_t1; static int kb_done;
static float kb_k(void) {
    static double dur = -1; if (dur < 0) { const char *e = getenv("RT_UPDATE_FADE"); dur = e ? atof(e) : 0.3; if (dur <= 0) dur = 0.3; }
    double k = kb_t0 ? (rt_now_ns() / 1e9 - kb_t0) / dur : 0;
    return k < 0 ? 0 : k > 1 ? 1 : (float)k;
}
int boot_kb_active(void) { return kb_t0 && !kb_done; }
void boot_kb_press(void) {                                  /* (rt_menu.c: 2 or A while it waits) */
    if (boot_kb_active() && !kb_t1 && kb_k() >= 1 && rt_now_ns() / 1e9 - kb_t0 > 0.8) { kb_t1 = rt_now_ns() / 1e9; rt_log("boot", "keyboard screen: 2 - on"); }
}
int boot_kb_hold(void) {                                    /* (rt_lyt.c: the boot asks for the title) */
    if (kb_done) return 0;
    static int force = -2; if (force == -2) { const char *e = getenv("RT_BOOT_KEYBOARD"); force = e ? atoi(e) : -1; }
    if (force == 0 || (force < 0 && getenv("RT_DETERMINISTIC")) || !keyboard_player() || !load_picture()) { kb_done = 1; return 0; }
    if (!kb_t0) { kb_t0 = rt_now_ns() / 1e9; rt_log("boot", "keyboard screen: after the console's - waits for 2"); }
    if (kb_t1) { kb_done = 1; return 0; }
    return 1;
}

/* before the matrices: the picture fills the box's inside, centred on it, as tall as its shape says */
void boot_layout_calc(uint32_t layout, uint32_t root) {
    (void)layout;
    uint32_t ci = cont_info(root);
    if (!ci) return;
    {   double t0, te; update_screen_times(&t0, &te);           /* the update screen: the text higher, nearer the panel's middle */
        if (t0) {
            if (upd_k() < 0.5f) return;                       /* (the old text, where it was, fading out) */
            uint32_t sh = lyt_find_pane(ci, "T_contInfo_00"), tx = lyt_find_pane(ci, "T_contInfo_01");
            if (sh) { wrf(sh + P_TRANS, 3); wrf(sh + P_TRANS + 4, UPD_TEXT_Y - 3); }
            if (tx) { wrf(tx + P_TRANS, 0); wrf(tx + P_TRANS + 4, UPD_TEXT_Y); }
            return;
        } }
    if (!kb_t0 || kb_k() < 0.5f) return;                     /* (the console's picture, as it is) */
    uint32_t pic = lyt_find_pane(ci, "P_yoko_00");
    if (!pic) return;
    float sx = rdf(pic + P_SCALE), sy = rdf(pic + P_SCALE + 4);                /* (the layout draws it at 0.7) */
    if (!(sx > 0.01f) || !(sy > 0.01f)) sx = sy = 1;
    wrf(pic + P_TRANS, 0); wrf(pic + P_TRANS + 4, 0);
    wrf(pic + P_SIZE, BOX_INSIDE / sx); wrf(pic + P_SIZE + 4, BOX_INSIDE * tex_h / tex_w / sy);
}
/* text boxes: our text (UTF-16, '\n' breaks the line) */
static void text_put(uint32_t tb, const char *s) {
    if (!tb) return;
    uint32_t buf = R32(tb + TB_TEXT); int cap = R16(tb + 0xFC) / 2 - 1, n = (int)strlen(s);
    if (!buf || cap < n) return;
    int same = R16(tb + 0xFE) == n;
    for (int i = 0; i < n && same; i++) if (R16(buf + 2 * i) != (uint8_t)s[i]) same = 0;
    if (same) return;
    for (int i = 0; i < n; i++) W16(buf + 2 * i, (uint8_t)s[i]);
    W16(buf + 2 * n, 0); W16(tb + 0xFE, (uint16_t)n);
}
/* ---- the update screen (rt_update.c holds the boot's title request; rt_menu.c update_screen)
 * is this layout again, without its picture box and "2" button: the text box says what is going
 * on, in the game's font, and the spinner / download bar are drawn over the box's place. The
 * layout is normally still up when the boot asks for the title (its fade to black comes with the
 * scene change, which waits): the picture box, the "2" button and the text fade out, the update's
 * text fades in, and the panel stays. Should the fader be black, it is faded back in (from
 * hle_DoSceneChange, which has a guest context). */
#define G_FADER     0x8042A440u                 /* dFader_c::mFader (vtable +0x10 getStatus: 0 opaque) */
#define A_SET_FADER 0x800B0DB0u                 /* dFader_c::setFader(type) */
#define A_FADE_IN   0x800B0A20u                 /* dFader_c::startFadeIn(frames) */
#define TB_FONTSIZE 0xE8                        /* TextBox: font size (w, h) */
const char *menu_update_text(void);             /* rt_menu.c */
void boot_update_frame(ppc_context *ctx, uint8_t *base) {
    static int faded; if (faded) return;
    faded = 1;
    uint32_t f = R32(G_FADER);                           /* (only if it is black: normally the layout is still up) */
    if (!f || rt_gcall(ctx, base, R32(R32(f) + 0x10), 1, f) != 0) return;
    rt_gcall(ctx, base, A_SET_FADER, 1, 0);
    rt_gcall(ctx, base, A_FADE_IN, 1, 20);
}
/* the line(s) for the text box: as one line up to ~25 letters at its size (32, 450 units wide),
   else two, broken at the space nearest the middle; smaller when a line is still too long */
/* how far the change from the controller screen is: 0 -> 1 over 0.3 s. The picture box and the
   "2" button fade out; the text fades out (to 0.5) and back in with the update's (from 0.5) */
static float upd_k(void) {
    double t0, te; update_screen_times(&t0, &te);
    static double dur = -1; if (dur < 0) { const char *e = getenv("RT_UPDATE_FADE"); dur = e ? atof(e) : 0.3; if (dur <= 0) dur = 0.3; }   /* (testing: slower) */
    double k = (rt_now_ns() / 1e9 - t0) / dur;
    return k < 0 ? 0 : k > 1 ? 1 : (float)k;
}
static uint8_t text_alpha[2]; static uint32_t text_alpha_pane[2];
static void update_text(uint32_t ci) {
    float k = upd_k();
    static const char *const boxes[] = {"T_contInfo_00", "T_contInfo_01"};
    uint32_t tbs[2];
    for (int i = 0; i < 2; i++) {
        tbs[i] = lyt_find_pane(ci, boxes[i]);
        if (tbs[i] && text_alpha_pane[i] != tbs[i]) { text_alpha_pane[i] = tbs[i]; text_alpha[i] = lyt_get_alpha(tbs[i]); }
        if (tbs[i]) lyt_set_alpha(tbs[i], (uint8_t)(text_alpha[i] * (k < 0.5f ? 1 - 2 * k : 2 * k - 1)));
    }
    static const char *const gone[] = {"W_yokomochi_00", "P_yoko_00", "N_btn_00", "N_2btn_00", "P_2btn_00", "P_2btn_01"};   /* (a pane's alpha doesn't always reach its children) */
    static uint32_t gone_pane[6]; static uint8_t gone_alpha[6];
    for (int i = 0; i < 6; i++) { uint32_t p = lyt_find_pane(ci, gone[i]); if (!p) continue;
        if (gone_pane[i] != p) { gone_pane[i] = p; gone_alpha[i] = lyt_get_alpha(p); }
        if (i >= 2 && k >= 0.5f) { lyt_set_visible(p, 0); continue; }    /* (the button's own animation sets its alpha: gone with the old text) */
        if (k < 1) lyt_set_alpha(p, (uint8_t)(gone_alpha[i] * (1 - k))); else lyt_set_visible(p, 0); }
    if (k < 0.5f) return;                                     /* (the old text is still fading out) */
    const char *m = menu_update_text();
    if (!m[0]) return;
    char t[220]; snprintf(t, sizeof t, "%s", m);
    int n = (int)strlen(t), longest = n;
    if (n > 25) {
        int best = -1;
        for (int i = 0; i < n; i++) if (t[i] == ' ' && (best < 0 || abs(i - n / 2) < abs(best - n / 2))) best = i;
        if (best > 0) { t[best] = '\n'; longest = best > n - best - 1 ? best : n - best - 1; }
    }
    float size = 32.0f * (longest > 25 ? 25.0f / longest : 1.0f);
    if (size < 18) size = 18;
    for (int i = 0; i < 2; i++) {
        uint32_t tb = tbs[i];
        if (!tb) continue;
        text_put(tb, t); wrf(tb + TB_FONTSIZE, size); wrf(tb + TB_FONTSIZE + 4, size);
    }
}
/* before the layout draws: our picture and text */
void boot_layout_draw(uint32_t layout, uint32_t root) {
    (void)layout;
    uint32_t ci = cont_info(root);
    if (!ci) return;
    {   static int tr = -1; if (tr < 0) tr = getenv("RT_BOOT_TRACE") != NULL; extern uint64_t g_frames_presented;
        if (tr && g_frames_presented % 15 == 0) rt_log("boot", "f%llu contInfo drawn: alpha %u/%u root alpha %u, update screen %d", (unsigned long long)g_frames_presented, R8(ci + 0xB8), R8(ci + 0xB9), R8(root + 0xB8), update_screen_active()); }
    static int said; if (!said) { said = 1; rt_log("boot", "controller screen: player 1 is %s", keyboard_player() ? "on the keyboard - the keyboard picture" : "a Wii Remote - the console's picture"); }
    {   static int dbg = -1; if (dbg < 0) dbg = getenv("RT_BOOT_DEBUG") != NULL;
        uint32_t p = lyt_find_pane(ci, "P_yoko_00");
        if (dbg == 1 && p) { dbg = 2; uint32_t mat = R32(p + PANE_MAT), tm = R32(mat + MAT_TEXMAP), tc = R32(p + 0xEC);
            rt_log("boot", "P_yoko_00: scale %.2f %.2f size %.1f %.1f texmap %08X %ux%u bits %08X lod %08X %08X", rdf(p + 0x44), rdf(p + 0x48), rdf(p + 0x4C), rdf(p + 0x50), R32(tm), R16(tm + 8), R16(tm + 10), R32(tm + 0x18), R32(tm + 12), R32(tm + 16));
            for (int i = 0; i < 0x70; i += 16) rt_log("boot", " mat+%02X: %08X %08X %08X %08X", i, R32(mat + i), R32(mat + i + 4), R32(mat + i + 8), R32(mat + i + 12));
            for (int i = 0; i < 32; i += 16) rt_log("boot", " tc+%02X: %08X %08X %08X %08X", i, R32(tc + i), R32(tc + i + 4), R32(tc + i + 8), R32(tc + i + 12)); } }
    {   double t0, te; update_screen_times(&t0, &te);           /* (from the update screen's start to the title) */
        if (t0) { update_text(ci); return; } }
    if (!kb_t0) return;                                       /* the console's screen first, as it is */
    float k = kb_k();
    {   static const char *const boxes[] = {"T_contInfo_00", "T_contInfo_01"};   /* the text: out, then ours in */
        for (int i = 0; i < 2; i++) { uint32_t tb = lyt_find_pane(ci, boxes[i]); if (!tb) continue;
            if (text_alpha_pane[i] != tb) { text_alpha_pane[i] = tb; text_alpha[i] = lyt_get_alpha(tb); }
            lyt_set_alpha(tb, (uint8_t)(text_alpha[i] * (k < 0.5f ? 1 - 2 * k : 2 * k - 1))); } }
    uint32_t pic = lyt_find_pane(ci, "P_yoko_00");
    if (pic) {                                                /* the picture: the same */
        static uint32_t pp; static uint8_t pa; if (pp != pic) { pp = pic; pa = lyt_get_alpha(pic); }
        lyt_set_alpha(pic, (uint8_t)(pa * (k < 0.5f ? 1 - 2 * k : 2 * k - 1)));
    }
    if (k < 0.5f) return;
    uint32_t mat = pic ? R32(pic + PANE_MAT) : 0, tm = mat ? R32(mat + MAT_TEXMAP) : 0;
    if (tm >= 0x80000000u && tm < 0x94000000u) {
        W32(tm, tex_addr); W16(tm + 8, (uint16_t)((tex_w + 3) & ~3)); W16(tm + 10, (uint16_t)((tex_h + 3) & ~3));
        W32(tm + 0x18, (R32(tm + 0x18) & 0x0FFFFFFFu) | (5u << 28));        /* GX_TF_RGB5A3 */
        for (int i = 0; i < 4; i++) W32(pic + PIC_VTX + 4 * i, 0xFFFFFFFFu);
    }
    text_put(lyt_find_pane(ci, "T_contInfo_00"), TEXT);
    text_put(lyt_find_pane(ci, "T_contInfo_01"), TEXT);
}
