/* rt_lyt.c - access to the game's NW4R layouts (2D screens) from the runtime.
 *
 * nw4r::lyt::Layout::Draw is hooked: before a layout draws, registered overrides can change its
 * panes (hide them, fade them). Pane layout in memory (nw4r 2009):
 *   +0x04 link {next, prev}   +0x0C parent   +0x10 children {count, next, prev}
 *   +0xB8 alpha  +0xB9 global alpha  +0xBB flags (bit 0 = visible)  +0xBC name[17]
 *   TextBox: +0xE4 font (nw4r::ut::ResFont)
 * RT_LYTDUMP=1 logs every layout's pane tree once. */
#include "rt.h"

#define P_CHILDREN 0x10
#define P_ALPHA 0xB8
#define P_FLAGS 0xBB
#define P_NAME 0xBC

static void pane_name(uint32_t p, char out[18]) { for (int i = 0; i < 17; i++) out[i] = (char)R8(p + P_NAME + i); out[17] = 0; }

/* depth-first search by name; 0 if not found */
uint32_t lyt_find_pane(uint32_t pane, const char *name) {
    if (!pane) return 0;
    char n[18]; pane_name(pane, n);
    if (!strcmp(n, name)) return pane;
    uint32_t sentinel = pane + P_CHILDREN + 4, node = R32(sentinel);
    for (int guard = 0; node && node != sentinel && guard < 512; guard++) {
        uint32_t r = lyt_find_pane(node - 4, name);
        if (r) return r;
        node = R32(node);
    }
    return 0;
}
void lyt_set_alpha(uint32_t pane, uint8_t a) { if (pane) W8(pane + P_ALPHA, a); }
uint8_t lyt_get_alpha(uint32_t pane) { return pane ? R8(pane + P_ALPHA) : 0; }
void lyt_set_visible(uint32_t pane, int v) { if (pane) W8(pane + P_FLAGS, (R8(pane + P_FLAGS) & ~1) | (v ? 1 : 0)); }

static void dump_tree(uint32_t pane, int depth, char *buf, size_t *n, size_t cap) {
    char nm[18]; pane_name(pane, nm);
    { float t[6]; for (int i = 0; i < 6; i++) { uint32_t u = R32(pane + (i < 2 ? 0x2C + 4 * i : 0x4C + 4 * (i - 2))); memcpy(&t[i], &u, 4); }
      *n += snprintf(buf + *n, cap - *n, "%*s%s (%08X a %u f %02X) t %.0f,%.0f s %.2f,%.2f", depth * 2, "", nm, pane, R8(pane + P_ALPHA), R8(pane + P_FLAGS), t[0], t[1], t[2], t[3]); }
    if (nm[0] == 'T' && nm[1] == '_') {                    /* text box: show fields + where its font data is */
        for (int o = 0xD8; o < 0x108; o += 4) *n += snprintf(buf + *n, cap - *n, " %X:%08X", o, R32(pane + o));
        uint32_t f = R32(pane + 0xE4);
        if (f >= 0x80000000u && f < 0x94000000u) for (int o = 0; o < 0x40; o += 4) { uint32_t q = R32(f + o);
            if (q >= 0x80000000u && q < 0x94000000u && R32(q) == 0x52464E54u) *n += snprintf(buf + *n, cap - *n, " RFNT@font+%X=%08X", o, q); }
    }
    *n += snprintf(buf + *n, cap - *n, "\n");
    if (*n > cap - 200) return;
    uint32_t sentinel = pane + P_CHILDREN + 4, node = R32(sentinel);
    for (int guard = 0; node && node != sentinel && guard < 512; guard++) { dump_tree(node - 4, depth + 1, buf, n, cap); node = R32(node); }
}

/* per-frame layout overrides live in rt_menu.c */
void menu_layout_hook(uint32_t layout, uint32_t root);

void hle_Draw__Q34nw4r3lyt6LayoutFRCQ34nw4r3lyt8DrawInfo(ppc_context *ctx, uint8_t *base) {
    uint32_t layout = ctx->r[3], root = R32(layout + 0x10);
    if (root) {
        static int dump = -1; if (dump < 0) dump = getenv("RT_LYTDUMP") != NULL;
        if (dump) {
            static uint32_t seen[256]; static int nseen; int f = 0;
            uint32_t key = root ^ (R32(root + 0x14) * 2654435761u);   /* roots get reused: include the first child */
            for (int i = 0; i < nseen; i++) if (seen[i] == key) f = 1;
            if (!f && nseen < 256) {
                seen[nseen++] = key;
                static char buf[65536]; size_t n = 0; dump_tree(root, 0, buf, &n, sizeof buf);
                rt_log("lyt", "layout %08X:\n%s", layout, buf);
            }
        }
        { static long fr = -2; if (fr == -2) { const char *e = getenv("RT_LYTFRAME"); fr = e ? atol(e) : -1; }
          extern uint64_t g_frames_presented;
          if (fr >= 0 && (long)g_frames_presented == fr) { char n1[18] = "", n2[18] = ""; uint32_t c = R32(root + 0x14);
              if (c && c != root + 0x14) { pane_name(c - 4, n1); uint32_t c2 = R32(c - 4 + 0x14); if (c2 && c2 != c - 4 + 0x14) pane_name(c2 - 4, n2); }
              rt_log("lyt", "frame %ld draws layout %08X root %08X: %s / %s", fr, layout, root, n1, n2); } }
        menu_layout_hook(layout, root);
    }
    orig_Draw__Q34nw4r3lyt6LayoutFRCQ34nw4r3lyt8DrawInfo(ctx, base);
}

/* dScene_c::setNextScene(profile, param, force): log scene changes (and who asked) */
void hle_DoSceneChange__FUsUiUi(ppc_context *ctx, uint8_t *base) {
    rt_log("scene", "next scene %u (param %08X%s) requested from %08X (%s)", ctx->r[3] & 0xFFFF, ctx->r[4], ctx->r[5] ? ", forced" : "", ctx->lr, rt_func_name(ctx->lr, NULL));
    orig_DoSceneChange__FUsUiUi(ctx, base);
}
void hle_setNextScene__10dScStage_cFUsiQ210dScStage_c6Exit_eQ28dFader_c12fader_type_e(ppc_context *ctx, uint8_t *base) {
    rt_log("scene", "stage exit to scene %u (r4 %08X r5 %08X r6 %08X) from %08X (%s)", ctx->r[3] & 0xFFFF, ctx->r[4], ctx->r[5], ctx->r[6], ctx->lr, rt_func_name(ctx->lr, NULL));
    orig_setNextScene__10dScStage_cFUsiQ210dScStage_c6Exit_eQ28dFader_c12fader_type_e(ctx, base);
}

/* nw4r::lyt::Layout::CalculateMtx: pane translations can be adjusted here, before the global
   matrices (and anything that reads them, like cursors) are computed */
void menu_layout_calc_hook(uint32_t layout, uint32_t root);
void hle_CalculateMtx__Q34nw4r3lyt6LayoutFRCQ34nw4r3lyt8DrawInfo(ppc_context *ctx, uint8_t *base) {
    uint32_t root = R32(ctx->r[3] + 0x10);
    if (root) menu_layout_calc_hook(ctx->r[3], root);
    orig_CalculateMtx__Q34nw4r3lyt6LayoutFRCQ34nw4r3lyt8DrawInfo(ctx, base);
}

/* BgTexMng::LoadAnimTile(this, tileset, tile, name, source, reverse): RT_ANIMLOG=1 logs each */
void hle__Z22BgTexMng__LoadAnimTilePvisPcS0_c(ppc_context *ctx, uint8_t *base) {
    static int on = -1; if (on < 0) on = getenv("RT_ANIMLOG") != NULL;
    if (on) { char a[40] = "", b[200] = ""; for (int i = 0; i < 39 && ctx->r[6] && R8(ctx->r[6] + i); i++) a[i] = (char)R8(ctx->r[6] + i);
              { int n = 0; for (int i = 0; i < 24 && ctx->r[7]; i++) n += snprintf(b + n, sizeof b - n, "%u,", R8(ctx->r[7] + i)); }
              rt_log("anim", "LoadAnimTile ts %d tile %d name '%s' src '%s'", (int)ctx->r[4], (int)(int16_t)ctx->r[5], a, b); }
    orig__Z22BgTexMng__LoadAnimTilePvisPcS0_c(ctx, base);
    if (on) rt_log("anim", "  -> %08X", ctx->r[3]);
}

/* SndAudioMgr::startSystemSe(id, ?): RT_SELOG=1 logs each (id, caller, scene) */
void hle_startSystemSe__11SndAudioMgrFUiUl(ppc_context *ctx, uint8_t *base) {
    static int on = -1; if (on < 0) on = getenv("RT_SELOG") != NULL;
    extern uint64_t g_frames_presented;
    extern uint32_t menu_filter_se(uint32_t id, uint32_t lr);
    uint32_t id = menu_filter_se(ctx->r[4], ctx->lr);
    if (on) rt_log("se", "frame %llu SE %u -> %u (r5 %u) from %08X (%s)", (unsigned long long)g_frames_presented, ctx->r[4], id, ctx->r[5], ctx->lr, rt_func_name(ctx->lr, NULL));
    if (!id) return;
    ctx->r[4] = id;
    orig_startSystemSe__11SndAudioMgrFUiUl(ctx, base);
}

/* dInfo_c::startGame(info): RT_WARP="world,level,area,entrance" (0-based world/level/area)
   replaces the first level started (testing). StartGameInfo_s: +5 entrance, +6 area,
   +12 world, +13 level. */
void hle_startGame__7dInfo_cFRCQ27dInfo_c15StartGameInfo_s(ppc_context *ctx, uint8_t *base) {
    static int done; const char *w = getenv("RT_WARP");
    uint32_t si = ctx->r[4];
    static int calls; calls++;
    if (w && !done && R8(si + 7) == 0 && calls >= 2) {       /* (the first start is the title) */
        int wd = 0, lv = 0, ar = 0, en = 0;
        if (sscanf(w, "%d,%d,%d,%d", &wd, &lv, &ar, &en) == 4) {
            W8(si + 12, (uint8_t)wd); W8(si + 13, (uint8_t)lv); W8(si + 14, (uint8_t)wd); W8(si + 15, (uint8_t)lv);
            W8(si + 6, (uint8_t)ar); W8(si + 5, (uint8_t)en); done = 1;
            rt_log("warp", "starting world %d level %d area %d entrance %d", wd + 1, lv + 1, ar, en);
        }
    }
    rt_log("scene", "startGame: world %u level %u area %u entrance %u demo %u (scene %u, from %08X %s)", R8(si + 12) + 1, R8(si + 13) + 1, R8(si + 6), R8(si + 5), R8(si + 7), R16(0x80428732u), ctx->lr, rt_func_name(ctx->lr, NULL));
    orig_startGame__7dInfo_cFRCQ27dInfo_c15StartGameInfo_s(ctx, base);
}

/* BGM entry points: RT_SELOG=1 logs them */
#define BGMLOG(fn) void hle_##fn(ppc_context *ctx, uint8_t *base) { \
    static int on = -1; if (on < 0) on = getenv("RT_SELOG") != NULL; extern uint64_t g_frames_presented; \
    if (on) rt_log("se", "frame %llu " #fn "(%08X, %08X, %08X) from %08X (%s)", (unsigned long long)g_frames_presented, ctx->r[3], ctx->r[4], ctx->r[5], ctx->lr, rt_func_name(ctx->lr, NULL)); \
    orig_##fn(ctx, base); }
BGMLOG(StartBGMMusic) BGMLOG(StopBGMMusic) BGMLOG(setMainBgmNo__6dAudioFi) BGMLOG(setBgmMode__6dAudioFi)
BGMLOG(fn_8019CC70)
