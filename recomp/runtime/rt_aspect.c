/* rt_aspect.c - hooks on the game's GX viewport / scissor / projection setup.
 * Step 1 (investigation): RT_VPLOG=1 logs every distinct call with its caller, so the
 * letterboxing code can be located. */
#include "rt.h"
#include <math.h>
extern uint64_t g_frames_presented;

/* 4:3 mode: settings.ini aspect_4_3 = 1 (default), RT_AR43 overrides */
static int ar43(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("RT_AR43"); m = e ? atoi(e) : rt_setting("aspect_4_3", 1); if (m) m = 2; }
    return m;
}
/* 16:9 mode: the game is told the console is set to 16:9 (SYSCONF IPL.AR), so it renders its
   native widescreen picture (anamorphic in the EFB) and the window shows it at 16:9 */
int rt_widescreen(void) { return !ar43(); }
void hle_SCGetAspectRatio(ppc_context *ctx, uint8_t *base) {
    orig_SCGetAspectRatio(ctx, base);
    ctx->r[3] = ar43() ? 0 : 1;
}
static void put_f(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); W32(a, u); }
static int vplog = -1;
static void log_once(const char *what, uint32_t lr, const char *fmt, ...) {
    if (vplog < 0) vplog = getenv("RT_VPLOG") != NULL;
    if (!vplog) return;
    static char seen[512][160]; static int nseen;
    char line[160]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    char key[160]; snprintf(key, sizeof key, "%s %08X %s", what, lr, line);
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], key)) return;
    if (nseen < 512) memcpy(seen[nseen++], key, sizeof key);
    rt_log("vp", "frame %llu %s from %08X (%s): %s", (unsigned long long)g_frames_presented, what, lr, rt_func_name(lr, NULL), line);
}

void hle_GXSetViewport(ppc_context *ctx, uint8_t *base) {
    log_once("viewport", ctx->lr, "%.1f %.1f %.1f %.1f z %.2f %.2f", ctx->f[1].d, ctx->f[2].d, ctx->f[3].d, ctx->f[4].d, ctx->f[5].d, ctx->f[6].d);
    orig_GXSetViewport(ctx, base);
}
void hle_GXSetScissor(ppc_context *ctx, uint8_t *base) {
    /* m2d layouts flagged to clip to the letterboxed picture (fn_80007010: world map HUD, prompts)
       get the whole screen in 4:3 mode, where the picture has no letterbox */
    if (ctx->lr == 0x80007198u && ar43()) { ctx->r[3] = 0; ctx->r[4] = 0; ctx->r[5] = 640; ctx->r[6] = 456; }
    log_once("scissor", ctx->lr, "%u %u %u %u", ctx->r[3], ctx->r[4], ctx->r[5], ctx->r[6]);
    orig_GXSetScissor(ctx, base);
}
void hle_GXSetProjection(ppc_context *ctx, uint8_t *base) {
    uint32_t m = ctx->r[3]; float v[16];
    for (int i = 0; i < 16; i++) { uint32_t u = R32(m + 4 * i); memcpy(&v[i], &u, 4); }
    log_once("projection", ctx->lr, "type %u  %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f", ctx->r[4], v[0], v[2], v[3], v[5], v[6], v[7], v[10], v[11]);
    orig_GXSetProjection(ctx, base);
}
/* nw4r::g3d::Camera::SetOrtho(t, b, l, r, near, far), SetViewport(x, y, w, h), SetScissor(x, y, w, h) */
void hle_SetOrtho__Q34nw4r3g3d6CameraFffffff(ppc_context *ctx, uint8_t *base) {
    log_once("cam.ortho", ctx->lr, "t %.2f b %.2f l %.2f r %.2f n %.1f f %.1f", ctx->f[1].d, ctx->f[2].d, ctx->f[3].d, ctx->f[4].d, ctx->f[5].d, ctx->f[6].d);
    orig_SetOrtho__Q34nw4r3g3d6CameraFffffff(ctx, base);
}
void hle_fn_80253D60(ppc_context *ctx, uint8_t *base) {
    log_once("cam.viewport", ctx->lr, "%.1f %.1f %.1f %.1f", ctx->f[1].d, ctx->f[2].d, ctx->f[3].d, ctx->f[4].d);
    orig_fn_80253D60(ctx, base);
}
void hle_fn_80253E60(ppc_context *ctx, uint8_t *base) {
    log_once("cam.scissor", ctx->lr, "%u %u %u %u", ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[7]);
    orig_fn_80253E60(ctx, base);
}
static void dump_obj(const char *what, uint32_t lr, uint32_t o, int words) {
    char buf[1200]; int n = 0;
    for (int i = 0; i < words && n < 1150; i++) { uint32_t u = R32(o + 4 * i); float f; memcpy(&f, &u, 4);
        n += (u > 0x80000000u && u < 0x94000000u) || u < 0x10000 ? snprintf(buf + n, sizeof buf - n, " %X:%08X", i * 4, u) : snprintf(buf + n, sizeof buf - n, " %X:%.2f", i * 4, f); }
    log_once(what, lr, "obj %08X%s", o, buf);
}
/* EGG::Frustum::CopyToG3D (ortho) and EGG::Screen viewport setup: dump the objects */
static void fix_letterbox_ortho(uint32_t fr);
void hle_fn_802C6EF0(ppc_context *ctx, uint8_t *base) { fix_letterbox_ortho(ctx->r[3]); dump_obj("frustum", ctx->lr, ctx->r[3], 24); orig_fn_802C6EF0(ctx, base); }
void hle_fn_802D1010(ppc_context *ctx, uint8_t *base) { dump_obj("screen", ctx->lr, ctx->r[3], 32); orig_fn_802D1010(ctx, base); }

/* mVideo::create sets the game's logical screen: ints 408 x 224 at r13-21916/-21912, floats
   408.704 x 224 at r13-21908/-21904 and their ratio at r13-21900. RT_AR43=1 keeps the width and
   makes the screen 4:3 (taller), RT_AR43=2 keeps the height (narrower). */
void hle_create__6mVideoFv(ppc_context *ctx, uint8_t *base) {
    orig_create__6mVideoFv(ctx, base);
    if (!ar43()) return;
    uint32_t sda = ctx->r[13];
    float w, h; uint32_t u;
    u = R32(sda - 21908); memcpy(&w, &u, 4); u = R32(sda - 21904); memcpy(&h, &u, 4);
    w = h * 4.0f / 3.0f;                     /* NSMBW's camera zoom levels are heights: keep them */
    W32(sda - 21916, (uint32_t)(int)w); W32(sda - 21912, (uint32_t)(int)h);
    put_f(sda - 21908, w); put_f(sda - 21904, h); put_f(sda - 21900, w / h);
    /* world <-> layout conversions assumed the letterboxed 4:3 picture (608 x 352 of 456):
       - getGlbPosToLyt: height = efb width * 0.548  -> * 0.7125 (= 456 / 640)
       - GetPosForLayoutEffect: 352                    -> 456
       - DispSizeScale: 0.85 (4:3 letterbox)           -> 1.0 */
    put_f(0x8042C58C, 456.0f / 640.0f);
    put_f(0x8042C590, 456.0f);
    put_f(0x8042C59C, 1.0f);
    rt_log("aspect", "4:3 mode: logical screen %.1f x %.1f, no letterbox", w, h);
}
/* the gameplay screen rectangle (virtual 608-wide canvas in 4:3): letterboxed 608x352 at y 52 -> full 608x456 */
void hle_GetSomeSizeRelatedBULLSHIT__Fv(ppc_context *ctx, uint8_t *base) {
    uint32_t lr = ctx->lr;
    orig_GetSomeSizeRelatedBULLSHIT__Fv(ctx, base);
    if (ar43() && ctx->r[3] == 0x44180000u) ctx->r[4] = 0x43E40000u;   /* 608 x 456 */
    log_once("sizeA", lr, "r3 %08X r4 %08X f1 %.3f f2 %.3f f3 %.3f f4 %.3f", ctx->r[3], ctx->r[4], ctx->f[1].d, ctx->f[2].d, ctx->f[3].d, ctx->f[4].d);
}
void hle_CalculateSomethingElseAboutRatio__Fv(ppc_context *ctx, uint8_t *base) {
    uint32_t lr = ctx->lr;
    orig_CalculateSomethingElseAboutRatio__Fv(ctx, base);
    if (ar43() && ctx->f[1].d > 0) ctx->f[1].d = 0;
    log_once("ratioB", lr, "f1 %.4f", ctx->f[1].d);
}
void hle_CalculateSomethingAboutRatio__Fffff(ppc_context *ctx, uint8_t *base) {
    uint32_t lr = ctx->lr, out = ctx->r[3];
    char in[200]; snprintf(in, sizeof in, "in f1 %.2f f2 %.2f f3 %.2f f4 %.2f r4 %08X r5 %08X", ctx->f[1].d, ctx->f[2].d, ctx->f[3].d, ctx->f[4].d, ctx->r[4], ctx->r[5]);
    orig_CalculateSomethingAboutRatio__Fffff(ctx, base);
    float o[4]; for (int i = 0; i < 4; i++) { uint32_t u = R32(out + 4 * i); memcpy(&o[i], &u, 4); }
    log_once("ratioA", lr, "%s -> %.3f %.3f %.3f %.3f", in, o[0], o[1], o[2], o[3]);
}

/* EGG::Screen position/size setter (screen, mode, x, y, w, h). The world map's camera setup
   (fn_2_1BEE40) letterboxes its two 3D screens in 4:3 (608 x 332 at y 62): give them the full
   608 x 456 picture instead (their perspective aspect follows the size). */
void hle_fn_802D1210(ppc_context *ctx, uint8_t *base) {
    if (ar43() && (ctx->lr == 0x8092C7D0u || ctx->lr == 0x8092C7ECu)) { ctx->f[2].d = 0; ctx->f[3].d = 608; ctx->f[4].d = 456; }
    orig_fn_802D1210(ctx, base);
}
/* letterboxed ortho screens (cutscenes: 608 x 352) whose world area is now 4:3 would stretch:
   make the horizontal world scale match the vertical one (the cutscene keeps its letterbox) */
static void fix_letterbox_ortho(uint32_t fr) {
    float sy, sx, h; uint32_t u;
    u = R32(fr + 0x0C); memcpy(&h, &u, 4);
    u = R32(fr + 0x28); memcpy(&sx, &u, 4); u = R32(fr + 0x2C); memcpy(&sy, &u, 4);
    if (ar43() && R32(fr + 4) == 0 && h > 300 && h < 400 && sx > 0 && sy > 0 && (sx < sy * 0.98f || sx > sy * 1.02f)) put_f(fr + 0x28, sy);
}

/* ---- 4:3: fixed cameras ----
   Small rooms (zones) use a static camera centered on the zone. The 16:9 picture shows a
   certain width around that center; in 4:3 the picture is narrower, so things at its sides -
   often the room's exit pipes - would be off screen and out of reach. For such cameras the view
   follows the players horizontally instead, kept within the width the 16:9 picture would have
   shown (and within the zone). Player positions come from daPyMng_c::getPlayer results. */
static uint32_t pl_ptr[8]; static uint64_t pl_seen[8];
void hle_getPlayer__9daPyMng_cFi(ppc_context *ctx, uint8_t *base) {
    uint32_t i = ctx->r[3];
    orig_getPlayer__9daPyMng_cFi(ctx, base);
    if (i < 8 && ctx->r[3]) { pl_ptr[i] = ctx->r[3]; pl_seen[i] = g_frames_presented; }
}
static float get_f(uint32_t a) { uint32_t u = R32(a); float f; memcpy(&f, &u, 4); return f; }
void aspect_camera_fix(void) {
    if (!ar43() || R32(0x8042A204u) == 2) return;          /* (not the title) */
    uint32_t bp = R32(0x80429E00u), bg = R32(0x80429DD0u);   /* dBgParameter_c, dBg_c */
    if (!bp || !bg) return;
    float zl = get_f(bg + 0x90030), zr = get_f(bg + 0x90034), w = get_f(bp + 0x40), lw = get_f(bp + 0x70), tx = get_f(bp + 0x48);
    float zw = zr - zl;
    if (zw <= 0 || w <= 0 || lw <= 0) return;
    float w169 = w / lw * 408.704f;                          /* the view width this zoom has in 16:9 */
    static float cur, lzl, lzr; static int active;
    float c = (zl + zr) * 0.5f;
    int fixed = fabsf(tx - c) < 1.0f && w < w169 - 1.0f && w < zw - 1.0f;
    { static int tr = -1; if (tr < 0) tr = getenv("RT_CAMTRACE") != NULL;
      if (tr && g_frames_presented % 60 == 0) rt_log("aspect", "zone %.0f-%.0f w %.1f lw %.1f tx %.1f w169 %.1f fixed %d players %08X/%llu", zl, zr, w, lw, tx, w169, fixed, pl_ptr[0], (unsigned long long)(g_frames_presented - pl_seen[0])); }
    if (!fixed) { active = 0; return; }
    float sx = 0; int n = 0;
    for (int i = 0; i < 8; i++) if (pl_ptr[i] && g_frames_presented - pl_seen[i] <= 2) { sx += get_f(pl_ptr[i] + 0xAC); n++; }
    if (!n) return;
    float want = sx / n - w * 0.5f;
    float lo = c - w169 * 0.5f, hi = c + w169 * 0.5f - w;          /* what the 16:9 picture covers */
    if (lo < zl) lo = zl; if (hi > zr - w) hi = zr - w;
    if (want < lo) want = lo; if (want > hi) want = hi;
    if (!active || zl != lzl || zr != lzr) { cur = want; lzl = zl; lzr = zr; active = 1; rt_log("aspect", "zone %.0f-%.0f: camera follows horizontally (4:3)", zl, zr); }
    else cur += (want - cur) * 0.2f;
    put_f(bp + 0x38, cur); put_f(bp + 0x60, cur); put_f(bg + 0x900AC, cur);
}

/* GXSetTexCopySrc(x, y, w, h): RT_VPLOG logs callers of letterbox-sized copies */
void hle_GXSetTexCopySrc(ppc_context *ctx, uint8_t *base) {
    log_once("copysrc", ctx->lr, "%u %u %u %u", ctx->r[3], ctx->r[4], ctx->r[5], ctx->r[6]);
    orig_GXSetTexCopySrc(ctx, base);
}

/* dGameCom::GetAspectRatio: 1 = 4:3 TV (the game letterboxes its 16:9 picture). In 4:3 mode the
   picture fills the screen, so code that sizes screen copies by it must see "no letterbox":
   - fn_8008A380: the layer-0 (hidden area) pass copies the picture to fade it */
void hle_GetAspectRatio__8dGameComFv(ppc_context *ctx, uint8_t *base) {
    uint32_t lr = ctx->lr;
    orig_GetAspectRatio__8dGameComFv(ctx, base);
    if (ar43() && (lr == 0x8008A404u)) ctx->r[3] = 1;    /* (here 0 takes the letterbox-band branch) */

    log_once("getaspect", lr, "-> %u", ctx->r[3]);
}
