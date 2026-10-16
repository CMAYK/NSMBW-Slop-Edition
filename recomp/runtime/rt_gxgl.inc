/* rt_gxgl.inc - OpenGL 3.3 backend for the GX renderer (included at the end of rt_gxrender.c).
 *
 * Shared with the software renderer: vertex decode, transform, lighting and texgen (CPU),
 * texture decoding + cache, EFB copy encoders. On the GPU: rasterization, TEV (one generated
 * GLSL program per TEV configuration, integer math mirroring the software path exactly),
 * alpha test, depth, blending, culling, scissor, EFB clears. The EFB is an offscreen
 * framebuffer (640x528, top row = EFB row 0 is at the top after a y flip in the vertex
 * transform); EFB copies read back only the copied rectangle. */
#include "rt_gl.h"
#include "rt_hdtex.h"

#define RT_GL_DEF(ret, name, args) PFN_##name name;
RT_GL_FUNCS(RT_GL_DEF)
#undef RT_GL_DEF

static GLuint gl_fbo, gl_color, gl_depth, gl_vao, gl_vbo, gl_ibo, gl_samplers[3][3][2], gl_samplers_hd[3][3];
static int gl_scale = 1;                 /* internal resolution multiplier (settings.ini render_scale) */
#define SW (EFB_W * gl_scale)
#define SH (EFB_H * gl_scale)
static size_t gl_vbo_size = 32u << 20, gl_vbo_off, gl_ibo_size = 8u << 20, gl_ibo_off;
static uint32_t efb_rb[EFB_W * EFB_H];    /* readback staging in EFB layout (0xAARRGGBB) */

static int gl_load(void) {
#define RT_GL_LOAD(ret, name, args) name = (PFN_##name)plat_gl_proc(#name); if (!name) { rt_log("gl", "missing %s", #name); return 0; }
    RT_GL_FUNCS(RT_GL_LOAD)
#undef RT_GL_LOAD
    return 1;
}

/* ------------------------------------------------------------------ shader generation */
typedef struct {
    uint32_t nst;                 /* stage count | ntex << 8 */
    uint32_t c[16], a[16];        /* TEV color/alpha combiner registers */
    uint8_t texen[16], coord[16], chan[16];
    uint32_t ksel[8];             /* konst selects + swap tables */
    uint32_t acmp;                /* alpha compare functions/op (refs are uniforms) */
    uint32_t nind, iref, ss0, ss1; /* indirect stages: count, map/coord refs, coordinate scales */
    uint32_t ind[16];             /* per TEV stage indirect command (BP 0x10+n) */
} tevkey;

/* uniform values of one draw (compared bytewise, so always memset before filling) */
typedef struct { GLint regs[16], kon[16], aref[2], cs[16], im[24], inddim[8], mapdim[32]; } gluni;

typedef struct { tevkey key; gluni ucache; int ucache_ok; GLuint prog; GLint u_reg, u_konst, u_aref, u_indmtx, u_cscale, u_mapdim, u_inddim; int used; } glprog;
#define NPROG 4096
static glprog progs[NPROG];
static GLuint gl_bound_prog;           /* program currently bound with glUseProgram */
static int nprogs;

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec4 aPos; layout(location=1) in vec4 aC0; layout(location=2) in vec4 aC1;\n"
    "layout(location=3) in vec3 aT0; layout(location=4) in vec3 aT1; layout(location=5) in vec3 aT2; layout(location=6) in vec3 aT3;\n"
    "layout(location=7) in vec3 aT4; layout(location=8) in vec3 aT5; layout(location=9) in vec3 aT6; layout(location=10) in vec3 aT7;\n"
    "out vec4 vC0; out vec4 vC1; out vec3 vT[8];\n"
    "void main() { gl_Position = aPos; vC0 = aC0; vC1 = aC1;\n"
    "  vT[0] = aT0; vT[1] = aT1; vT[2] = aT2; vT[3] = aT3; vT[4] = aT4; vT[5] = aT5; vT[6] = aT6; vT[7] = aT7; }\n";

typedef struct { char *s; size_t n, cap; } sbuf;
static void sb(sbuf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[1024]; int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (b->n + n + 1 > b->cap) { b->cap = (b->cap + n + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->n, tmp, n + 1); b->n += n;
}

static const char *ccin(int sel) {          /* color combiner inputs (ivec3) */
    static const char *t[16] = {"R[0].rgb", "ivec3(R[0].a)", "R[1].rgb", "ivec3(R[1].a)", "R[2].rgb", "ivec3(R[2].a)",
                                "R[3].rgb", "ivec3(R[3].a)", "tex.rgb", "ivec3(tex.a)", "ras.rgb", "ivec3(ras.a)",
                                "ivec3(255)", "ivec3(128)", "kc.rgb", "ivec3(0)"};
    return t[sel & 15];
}
static const char *acin(int sel) {
    static const char *t[8] = {"R[0].a", "R[1].a", "R[2].a", "R[3].a", "tex.a", "ras.a", "ka", "0"};
    return t[sel & 7];
}
static void konst_expr(char *out, size_t n, int sel, int alpha) {
    static const int v[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    static const char comp[4] = {'r', 'g', 'b', 'a'};
    if (sel < 8) { snprintf(out, n, alpha ? "%d" : "ivec4(%d)", v[sel]); return; }
    if (!alpha && sel >= 0x0C && sel <= 0x0F) { snprintf(out, n, "uKonst[%d]", sel - 0x0C); return; }
    if (sel < 0x10) { snprintf(out, n, alpha ? "0" : "ivec4(0)"); return; }
    int k = sel & 3, c = (sel - 0x10) >> 2;
    snprintf(out, n, alpha ? "uKonst[%d].%c" : "ivec4(uKonst[%d].%c)", k, comp[c]);
}
static const char *cmpexpr(int f) {          /* alpha compare against ref (%s placeholders filled by caller) */
    static const char *t[8] = {"false", "(a < %s)", "(a == %s)", "(a <= %s)", "(a > %s)", "(a != %s)", "(a >= %s)", "true"};
    return t[f & 7];
}

static GLuint gl_compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); rt_log("gl", "shader compile failed:\n%s\n%s", log, src); }
    return s;
}

/* ---- shader cache (game/shadercache.bin): linked programs saved as driver binaries, so a TEV
   configuration is compiled once ever instead of causing a hitch the first time it shows up in
   each session. Keyed by a hash of the generated shader sources; dropped when the driver
   changes. Needs GL_ARB_get_program_binary (optional). */
typedef void (GLAPIENTRY *PFN_glProgramBinary_)(GLuint, GLenum, const void *, GLsizei);
typedef void (GLAPIENTRY *PFN_glGetProgramBinary_)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
typedef void (GLAPIENTRY *PFN_glProgramParameteri_)(GLuint, GLenum, GLint);
static PFN_glProgramBinary_ p_glProgramBinary; static PFN_glGetProgramBinary_ p_glGetProgramBinary; static PFN_glProgramParameteri_ p_glProgramParameteri;
typedef struct { uint64_t h; uint32_t fmt, len; uint8_t *data; } pcent;
static pcent *pcache; static int npcache, cappcache, pcache_state;   /* state: 0 not loaded, 1 on, -1 off */
static char pcache_path[1200];
static uint64_t pcache_driver;
static uint64_t fnv64(const char *s, uint64_t h) { for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001B3ull; return h; }
static void pcache_load(void) {
    pcache_state = -1;
    p_glProgramBinary = (PFN_glProgramBinary_)plat_gl_proc("glProgramBinary");
    p_glGetProgramBinary = (PFN_glGetProgramBinary_)plat_gl_proc("glGetProgramBinary");
    p_glProgramParameteri = (PFN_glProgramParameteri_)plat_gl_proc("glProgramParameteri");
    if (!p_glProgramBinary || !p_glGetProgramBinary || !p_glProgramParameteri || getenv("RT_NOSHADERCACHE")) return;
    pcache_driver = fnv64((const char *)glGetString(GL_RENDERER), fnv64((const char *)glGetString(GL_VERSION), 0xCBF29CE484222325ull));
    snprintf(pcache_path, sizeof pcache_path, "%s/shadercache.bin", g_root);
    pcache_state = 1;
    FILE *f = fopen(pcache_path, "rb");
    if (!f) return;
    char magic[8]; uint64_t drv;
    if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, "NSPCACHE", 8) && fread(&drv, 8, 1, f) == 1 && drv == pcache_driver) {
        for (;;) {
            pcent e; if (fread(&e.h, 8, 1, f) != 1 || fread(&e.fmt, 4, 1, f) != 1 || fread(&e.len, 4, 1, f) != 1 || e.len > (16u << 20)) break;
            e.data = malloc(e.len); if (fread(e.data, 1, e.len, f) != e.len) { free(e.data); break; }
            if (npcache == cappcache) { cappcache = cappcache ? cappcache * 2 : 256; pcache = realloc(pcache, sizeof *pcache * cappcache); }
            pcache[npcache++] = e;
        }
        rt_log("gl", "shader cache: %d programs", npcache);
    } else { fclose(f); f = NULL; remove(pcache_path); rt_log("gl", "shader cache: new (driver changed or first run)"); }
    if (f) fclose(f);
}
static GLuint pcache_get(uint64_t h) {
    for (int i = 0; i < npcache; i++) if (pcache[i].h == h) {
        GLuint pr = glCreateProgram();
        p_glProgramBinary(pr, pcache[i].fmt, pcache[i].data, (GLsizei)pcache[i].len);
        GLint ok = 0; glGetProgramiv(pr, GL_LINK_STATUS, &ok);
        if (ok) return pr;
        glDeleteProgram(pr); return 0;                       /* (rejected by the driver: compile again) */
    }
    return 0;
}
static void pcache_put(uint64_t h, GLuint pr) {
    GLint len = 0; glGetProgramiv(pr, 0x8741 /* GL_PROGRAM_BINARY_LENGTH */, &len);
    if (len <= 0 || len > (16 << 20)) return;
    pcent e; e.h = h; e.data = malloc((size_t)len); GLsizei got = 0; GLenum fmt = 0;
    p_glGetProgramBinary(pr, len, &got, &fmt, e.data);
    if (got <= 0) { free(e.data); while (glGetError()) {} return; }
    e.fmt = fmt; e.len = (uint32_t)got;
    if (npcache == cappcache) { cappcache = cappcache ? cappcache * 2 : 256; pcache = realloc(pcache, sizeof *pcache * cappcache); }
    pcache[npcache++] = e;
    FILE *f = fopen(pcache_path, "r+b");
    if (!f) { f = fopen(pcache_path, "wb"); if (!f) return; fwrite("NSPCACHE", 1, 8, f); fwrite(&pcache_driver, 8, 1, f); }
    else fseek(f, 0, SEEK_END);
    fwrite(&e.h, 8, 1, f); fwrite(&e.fmt, 4, 1, f); fwrite(&e.len, 4, 1, f); fwrite(e.data, 1, e.len, f);
    fclose(f);
}

static glprog *gl_program(const tevkey *k) {
    uint32_t h = 2166136261u; const uint8_t *p = (const uint8_t *)k;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ p[i]) * 16777619u;
    for (int probe = 0; probe < NPROG; probe++) {
        glprog *g = &progs[(h + probe) % NPROG];
        if (g->used && !memcmp(&g->key, k, sizeof *k)) return g;
        if (!g->used) {
            if (nprogs >= NPROG - 1) return NULL;
            /* generate the fragment shader */
            int nst = k->nst & 255;
            sbuf b = {0};
            sb(&b, "#version 330 core\nin vec4 vC0; in vec4 vC1; in vec3 vT[8];\nout vec4 oColor;\n"
                   "uniform ivec4 uReg[4]; uniform ivec4 uKonst[4]; uniform ivec2 uAref;\n");
            for (int s = 0; s < nst; s++) if (k->texen[s]) sb(&b, "uniform sampler2D uT%d;\n", s);
            int nind = k->nind, ntexk = (k->nst >> 8) & 15;
            for (int i = 0; i < nind; i++) sb(&b, "uniform sampler2D uI%d;\n", i);
            sb(&b, "uniform ivec4 uIndMtx[6]; uniform ivec2 uCScale[8]; uniform ivec2 uMapDim[16]; uniform ivec2 uIndDim[4];\n");
            sb(&b, "int tclamp(int v, bool c) { return c ? clamp(v, 0, 255) : clamp(v, -1024, 1023); }\n"
                   "ivec3 tclamp3(ivec3 v, bool c) { return c ? clamp(v, ivec3(0), ivec3(255)) : clamp(v, ivec3(-1024), ivec3(1023)); }\n"
                   "int half_(int v) { return v < 0 ? -((-v) >> 1) : (v >> 1); }\n"
                   "ivec3 half3(ivec3 v) { return ivec3(half_(v.x), half_(v.y), half_(v.z)); }\n"
                   "void main() {\n  ivec4 R[4] = ivec4[4](uReg[0], uReg[1], uReg[2], uReg[3]);\n"
                   "  ivec4 col0 = ivec4(vC0 * 255.0 + 0.5); ivec4 col1 = ivec4(vC1 * 255.0 + 0.5);\n  ivec2 tevcoord = ivec2(0);\n");
            /* texture coordinates in fixed point (1/128 texel of the coordinate's scale), as the hardware does */
            for (int c = 0; c < ntexk; c++)
                sb(&b, "  ivec2 fx%d = ivec2((vT[%d].z != 0.0 ? vT[%d].xy / vT[%d].z : vT[%d].xy) * vec2(uCScale[%d]));\n", c, c, c, c, c, c);
            for (int i = 0; i < nind; i++) {
                int coord = (k->iref >> (6 * i + 3)) & 7; if (coord >= ntexk) coord = 0;
                uint32_t ss = i < 2 ? k->ss0 >> (8 * i) : k->ss1 >> (8 * (i - 2));
                if (ntexk == 0) { sb(&b, "  ivec3 iind%d = ivec3(0);\n", i); continue; }
                sb(&b, "  ivec3 iind%d; { ivec2 tc = ivec2(fx%d.x >> %d, fx%d.y >> %d);\n"
                       "    ivec4 ti = ivec4(texture(uI%d, vec2(tc) / vec2(uIndDim[%d] * 128)) * 255.0 + 0.5); iind%d = ivec3(ti.a, ti.b, ti.g); }\n",
                   i, coord, ss & 15, coord, (ss >> 4) & 15, i, i, i);
            }
            for (int s = 0; s < nst; s++) {
                uint32_t c = k->c[s], a = k->a[s];
                int cd = c & 15, cc = (c >> 4) & 15, cb = (c >> 8) & 15, ca = (c >> 12) & 15;
                int cbias = (c >> 16) & 3, cop = (c >> 18) & 1, cclamp = (c >> 19) & 1, cscale = (c >> 20) & 3, cdst = (c >> 22) & 3;
                int ad = (a >> 4) & 7, ac = (a >> 7) & 7, ab = (a >> 10) & 7, aa = (a >> 13) & 7;
                int abias = (a >> 16) & 3, aop = (a >> 18) & 1, aclamp = (a >> 19) & 1, ascale = (a >> 20) & 3, adst = (a >> 22) & 3;
                int rsw = a & 3, tsw = (a >> 2) & 3;
                uint32_t r0 = k->ksel[rsw * 2], r1 = k->ksel[rsw * 2 + 1], t0 = k->ksel[tsw * 2], t1 = k->ksel[tsw * 2 + 1];
                int rs[4] = {(int)(r0 & 3), (int)((r0 >> 2) & 3), (int)(r1 & 3), (int)((r1 >> 2) & 3)};
                int ts[4] = {(int)(t0 & 3), (int)((t0 >> 2) & 3), (int)(t1 & 3), (int)((t1 >> 2) & 3)};
                static const char sw[4] = {'r', 'g', 'b', 'a'};
                uint32_t ksel = k->ksel[s / 2] >> ((s & 1) ? 14 : 4);
                char kce[64], kae[64];
                konst_expr(kce, sizeof kce, ksel & 31, 0); konst_expr(kae, sizeof kae, (ksel >> 5) & 31, 1);
                sb(&b, "  { // stage %d\n    ivec4 tex = ivec4(255); ivec4 ras = ivec4(0);\n", s);
                uint32_t ind = k->ind[s];
                int crd = k->coord[s];
                if (ind && ntexk) {
                    /* indirect texturing (Dolphin PixelShaderGen semantics) */
                    int bt = ind & 3, fmt = (ind >> 2) & 3, bias = (ind >> 4) & 7, bs = (ind >> 7) & 3, mid = (ind >> 9) & 15;
                    int wsw = (ind >> 13) & 7, wtw = (ind >> 16) & 7, addprev = (ind >> 20) & 1;
                    static const int fmask[4] = {255, 31, 15, 7};
                    sb(&b, "    { ivec2 tr = ivec2(0);\n");
                    if (bt < nind && (mid != 0 || bs != 0)) {
                        sb(&b, "      ivec3 crd = iind%d & %d;\n", bt, fmask[fmt]);
                        const char *badd = fmt == 0 ? "-128" : "1";
                        if (bias & 1) sb(&b, "      crd.x += %s;\n", badd);
                        if (bias & 2) sb(&b, "      crd.y += %s;\n", badd);
                        if (bias & 4) sb(&b, "      crd.z += %s;\n", badd);
                        int m = -1;
                        if (mid >= 1 && mid <= 3) { m = mid - 1;
                            sb(&b, "      tr = ivec2(uIndMtx[%d].x * crd.x + uIndMtx[%d].y * crd.y + uIndMtx[%d].z * crd.z,"
                                   " uIndMtx[%d].x * crd.x + uIndMtx[%d].y * crd.y + uIndMtx[%d].z * crd.z) >> 3;\n", 2 * m, 2 * m, 2 * m, 2 * m + 1, 2 * m + 1, 2 * m + 1); }
                        else if (mid >= 5 && mid <= 7) { m = mid - 5; sb(&b, "      tr = (fx%d * crd.xx) >> 8;\n", crd); }
                        else if (mid >= 9 && mid <= 11) { m = mid - 9; sb(&b, "      tr = (fx%d * crd.yy) >> 8;\n", crd); }
                        if (m >= 0) sb(&b, "      { int sh = uIndMtx[%d].w; tr = sh >= 0 ? (tr >> sh) : (tr << (-sh)); }\n", 2 * m);
                    }
                    sb(&b, "      ivec2 wc = fx%d;\n", crd);
                    if (wsw == 6) sb(&b, "      wc.x = 0;\n"); else if (wsw) sb(&b, "      wc.x &= %d;\n", ((256 >> (wsw - 1)) << 7) - 1);
                    if (wtw == 6) sb(&b, "      wc.y = 0;\n"); else if (wtw) sb(&b, "      wc.y &= %d;\n", ((256 >> (wtw - 1)) << 7) - 1);
                    sb(&b, addprev ? "      tevcoord = tevcoord + wc + tr; }\n" : "      tevcoord = wc + tr; }\n");
                    if (k->texen[s])
                        sb(&b, "    { ivec4 tv = ivec4(texture(uT%d, vec2(tevcoord) / vec2(uMapDim[%d] * 128)) * 255.0 + 0.5); tex = ivec4(tv.%c, tv.%c, tv.%c, tv.%c); }\n",
                           s, s, sw[ts[0]], sw[ts[1]], sw[ts[2]], sw[ts[3]]);
                } else if (k->texen[s]) {
                    if (crd < ntexk) sb(&b, "    tevcoord = fx%d;\n", crd);
                    sb(&b, "    { vec3 t3 = vT[%d]; vec2 st = t3.z != 0.0 ? t3.xy / t3.z : t3.xy;\n"
                           "      ivec4 tv = ivec4(texture(uT%d, st) * 255.0 + 0.5); tex = ivec4(tv.%c, tv.%c, tv.%c, tv.%c); }\n",
                       crd, s, sw[ts[0]], sw[ts[1]], sw[ts[2]], sw[ts[3]]);
                }
                if (k->chan[s] <= 1)
                    sb(&b, "    ras = ivec4(col%d.%c, col%d.%c, col%d.%c, col%d.%c);\n",
                       k->chan[s], sw[rs[0]], k->chan[s], sw[rs[1]], k->chan[s], sw[rs[2]], k->chan[s], sw[rs[3]]);
                sb(&b, "    ivec4 kc = %s; int ka = %s;\n", kce, kae);
                sb(&b, "    ivec3 A = %s, B = %s, C = %s, D = %s;\n", ccin(ca), ccin(cb), ccin(cc), ccin(cd));
                sb(&b, "    int aA = %s, aB = %s, aC = %s, aD = %s;\n", acin(aa), acin(ab), acin(ac), acin(ad));
                if (cbias != 3) {
                    sb(&b, "    ivec3 a3 = A & 255, b3 = B & 255, c3 = C & 255; ivec3 cc3 = c3 + (c3 >> 7);\n"
                           "    ivec3 v = D %c ((a3 * (256 - cc3) + b3 * cc3) >> 8);\n", cop ? '-' : '+');
                    if (cbias == 1) sb(&b, "    v += 128;\n"); else if (cbias == 2) sb(&b, "    v -= 128;\n");
                    if (cscale == 1) sb(&b, "    v *= 2;\n"); else if (cscale == 2) sb(&b, "    v *= 4;\n"); else if (cscale == 3) sb(&b, "    v = half3(v);\n");
                    sb(&b, "    R[%d].rgb = tclamp3(v, %s);\n", cdst, cclamp ? "true" : "false");
                } else {
                    sb(&b, "    ivec3 a3 = A & 255, b3 = B & 255, c3 = C & 255; bvec3 g;\n");
                    const char *rel = cop ? "==" : ">";
                    switch (cscale) {
                    case 0: sb(&b, "    g = bvec3(a3.r %s b3.r);\n", rel); break;
                    case 1: sb(&b, "    g = bvec3((a3.r | (a3.g << 8)) %s (b3.r | (b3.g << 8)));\n", rel); break;
                    case 2: sb(&b, "    g = bvec3((a3.r | (a3.g << 8) | (a3.b << 16)) %s (b3.r | (b3.g << 8) | (b3.b << 16)));\n", rel); break;
                    default: sb(&b, cop ? "    g = equal(a3, b3);\n" : "    g = greaterThan(a3, b3);\n"); break;
                    }
                    sb(&b, "    R[%d].rgb = tclamp3(D + ivec3(g) * c3, %s);\n", cdst, cclamp ? "true" : "false");
                }
                if (abias != 3) {
                    sb(&b, "    { int a1 = aA & 255, b1 = aB & 255, c1 = aC & 255; int cc1 = c1 + (c1 >> 7);\n"
                           "      int v = aD %c ((a1 * (256 - cc1) + b1 * cc1) >> 8);\n", aop ? '-' : '+');
                    if (abias == 1) sb(&b, "      v += 128;\n"); else if (abias == 2) sb(&b, "      v -= 128;\n");
                    if (ascale == 1) sb(&b, "      v *= 2;\n"); else if (ascale == 2) sb(&b, "      v *= 4;\n"); else if (ascale == 3) sb(&b, "      v = half_(v);\n");
                    sb(&b, "      R[%d].a = tclamp(v, %s); }\n", adst, aclamp ? "true" : "false");
                } else {
                    const char *rel = aop ? "==" : ">";
                    sb(&b, "    { ivec3 a3 = A & 255, b3 = B & 255; bool g;\n");
                    switch (ascale) {
                    case 0: sb(&b, "      g = a3.r %s b3.r;\n", rel); break;
                    case 1: sb(&b, "      g = (a3.r | (a3.g << 8)) %s (b3.r | (b3.g << 8));\n", rel); break;
                    case 2: sb(&b, "      g = (a3.r | (a3.g << 8) | (a3.b << 16)) %s (b3.r | (b3.g << 8) | (b3.b << 16));\n", rel); break;
                    default: sb(&b, "      g = (aA & 255) %s (aB & 255);\n", rel); break;
                    }
                    sb(&b, "      R[%d].a = tclamp(aD + (g ? (aC & 255) : 0), %s); }\n", adst, aclamp ? "true" : "false");
                }
                sb(&b, "  }\n");
            }
            sb(&b, "  ivec4 o = clamp(R[0], ivec4(0), ivec4(255)); int a = o.a;\n");
            int f0 = (k->acmp >> 16) & 7, f1 = (k->acmp >> 19) & 7, op = (k->acmp >> 22) & 3;
            int always = (op == 0 && f0 == 7 && f1 == 7) || (op == 1 && (f0 == 7 || f1 == 7));
            if (!always) {
                char e0[64], e1[64];
                snprintf(e0, sizeof e0, cmpexpr(f0), "uAref.x"); snprintf(e1, sizeof e1, cmpexpr(f1), "uAref.y");
                static const char *ops[4] = {"(%s && %s)", "(%s || %s)", "(%s != %s)", "(%s == %s)"};
                char pass[160]; snprintf(pass, sizeof pass, ops[op], e0, e1);
                sb(&b, "  if (!%s) discard;\n", pass);
            }
            sb(&b, "  oColor = vec4(o) / 255.0;\n}\n");

            extern uint64_t g_compile_ns; uint64_t tc0 = rt_now_ns();
            if (!pcache_state) pcache_load();
            uint64_t ph = fnv64(b.s, fnv64(VS, 0xCBF29CE484222325ull));
            GLuint pr = pcache_state > 0 ? pcache_get(ph) : 0;
            if (!pr) {
                static GLuint vs;
                if (!vs) vs = gl_compile(GL_VERTEX_SHADER, VS);
                GLuint fs = gl_compile(GL_FRAGMENT_SHADER, b.s);
                pr = glCreateProgram();
                if (pcache_state > 0) p_glProgramParameteri(pr, 0x8257 /* GL_PROGRAM_BINARY_RETRIEVABLE_HINT */, 1);
                glAttachShader(pr, vs); glAttachShader(pr, fs); glLinkProgram(pr);
                GLint ok = 0; glGetProgramiv(pr, GL_LINK_STATUS, &ok);
                if (!ok) { char log[2048]; glGetProgramInfoLog(pr, sizeof log, NULL, log); rt_log("gl", "link failed: %s", log); }
                glDeleteShader(fs);
                if (ok && pcache_state > 0) pcache_put(ph, pr);
            }
            g_compile_ns += rt_now_ns() - tc0;
            free(b.s);
            g->key = *k; g->prog = pr; g->used = 1; nprogs++;
            glUseProgram(pr); gl_bound_prog = pr;
            g->u_reg = glGetUniformLocation(pr, "uReg"); g->u_konst = glGetUniformLocation(pr, "uKonst"); g->u_aref = glGetUniformLocation(pr, "uAref");
            g->u_indmtx = glGetUniformLocation(pr, "uIndMtx"); g->u_cscale = glGetUniformLocation(pr, "uCScale");
            g->u_mapdim = glGetUniformLocation(pr, "uMapDim"); g->u_inddim = glGetUniformLocation(pr, "uIndDim");
            for (int i = 0; i < nind; i++) { char nm[8]; snprintf(nm, sizeof nm, "uI%d", i); glUniform1i(glGetUniformLocation(pr, nm), 8 + i); }
            for (int s = 0; s < nst; s++) if (k->texen[s]) { char nm[8]; snprintf(nm, sizeof nm, "uT%d", s); glUniform1i(glGetUniformLocation(pr, nm), s); }
            if (nprogs % 50 == 0) LOG("gl", "%d TEV programs", nprogs);
            return g;
        }
    }
    return NULL;
}


/* ------------------------------------------------------------------ batching
 * Each draw's complete GL state (program, uniforms, textures, raster state) is captured in a
 * plain struct. Consecutive draws with identical state are appended to one CPU-side vertex +
 * index buffer and submitted with a single glDrawElements. The state is only recomputed when a
 * BP register changed since the previous draw (g_gx_state_gen). A batch is flushed on a state
 * change, before EFB reads/clears/copies, before a texture it might use is re-uploaded, and
 * when the buffers fill up. */
typedef struct {
    glprog *pg;
    int ntex;
    gluni u;
    GLuint tex[12], smp[12];       /* units 0-7: TEV stages, 8-11: indirect maps */
    int blend, eq; GLenum sf, df, sfa, dfa; int bconst, dstconst_en; int cmask; int zen, zfn, zupd; int cull; int sc[4];
} glstate;

static glstate gl_cur, gl_bst;          /* current draw state / state of the pending batch */
static uint32_t gl_cur_gen = 0xFFFFFFFFu; static int gl_cur_ok;
static int bpending;
static uint8_t *bvbuf; static size_t bvlen, bvcap; static uint32_t bvcount;
static uint32_t *bibuf; static size_t bilen, bicap;
static GLuint bound_tex[16], bound_smp[16]; static int active_unit = -1;
static int enabled_tex = -1;
extern uint32_t g_gx_state_gen;
uint64_t g_perf_flush_ns; uint32_t g_perf_batches;
static void gl_flush(void);
static GLuint gc_lookup(int map, uint32_t *w, uint32_t *h);

/* ------------------------------------------------------------------ textures */
/* texture filtering (options menu): 0 nearest, 1 bilinear (as the game asks), 2..5 anisotropic
   2x/4x/8x/16x (game textures get mipmaps) */
static int gl_filter = 1;
static void gl_apply_filter(void);
typedef struct { GLuint tex; uint64_t ver, pver; } gltexslot;
static gltexslot gltex[TEXCACHE];
static void gl_unit(int u) { if (active_unit != u) { glActiveTexture(GL_TEXTURE0 + u); active_unit = u; } }
/* upload a loaded custom texture (all its mip levels, or generated ones) */
static GLuint hd_upload(hdtex *e) {
    GLuint t; glGenTextures(1, &t);
    gl_unit(15); glBindTexture(GL_TEXTURE_2D, t); bound_tex[15] = t;
    while (glGetError()) {}
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    int compressed = e->fmt >= HD_BC1;
    for (int l = 0; l < e->nlev; l++) {
        const uint8_t *d = e->data + e->lev[l].off;
        if (!compressed) glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, e->lev[l].w, e->lev[l].h, 0, e->fmt == HD_RGBA8 ? GL_RGBA : GL_BGRA, GL_UNSIGNED_BYTE, d);
        else {
            static const GLenum cf[] = {0, 0, 0, 0x83F1, 0x83F2, 0x83F3, 0x8E8C};
            glCompressedTexImage2D(GL_TEXTURE_2D, l, cf[e->fmt], e->lev[l].w, e->lev[l].h, 0, (GLsizei)e->lev[l].size, d);
        }
    }
    if (e->nlev == 1 && !compressed) glGenerateMipmap(GL_TEXTURE_2D);
    else glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, e->nlev - 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    free(e->data); e->data = NULL;
    if (glGetError()) { glDeleteTextures(1, &t); bound_tex[15] = 0; e->state = HD_FAILED; rt_log("hdtex", "GPU rejected %s", e->path); return 0; }
    e->gltex = t; e->state = HD_UPLOADED;
    return t;
}
static GLuint gl_texture(texent *t, int *is_hd) {
    *is_hd = 0;
    if (t->hd) {                              /* custom texture from a pack */
        hdtex *e = t->hd;
        if (e->state == HD_UPLOADED) { *is_hd = 1; return e->gltex; }
        if (e->state == HD_READY) { GLuint h = hd_upload(e); if (h) { *is_hd = 1; return h; } }
        else if (e->state == HD_IDLE) hd_request(e);
    }
    gltexslot *g = &gltex[t - texcache];
    if (!g->tex) glGenTextures(1, &g->tex);
    if (g->ver != t->ver) {                   /* (re)decoded since last upload */
        if (bpending) gl_flush();             /* the pending batch may still sample the old contents */
        gl_unit(15); glBindTexture(GL_TEXTURE_2D, g->tex); bound_tex[15] = g->tex;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->w, t->h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, t->px);
        if (gl_filter >= 2) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000); glGenerateMipmap(GL_TEXTURE_2D); }
        else glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        g->ver = t->ver; g->pver = t->pver; t->upd_y0 = t->upd_y1 = 0;
    } else if (g->pver != t->pver && t->upd_y1 > t->upd_y0) {   /* some rows changed in place */
        if (bpending) gl_flush();
        gl_unit(15); glBindTexture(GL_TEXTURE_2D, g->tex); bound_tex[15] = g->tex;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)t->upd_y0, (GLsizei)t->w, (GLsizei)(t->upd_y1 - t->upd_y0), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, t->px + (size_t)t->upd_y0 * t->w);
        if (gl_filter >= 2) glGenerateMipmap(GL_TEXTURE_2D);
        g->pver = t->pver; t->upd_y0 = t->upd_y1 = 0;
    }
    return g->tex;
}

/* ------------------------------------------------------------------ init */
static int gl_init(void) {
    if (!plat_gl_create() || !gl_load()) return 0;
    gl_scale = rt_setting("render_scale", 2); if (gl_scale < 1) gl_scale = 1; if (gl_scale > 8) gl_scale = 8;
    rt_log("gl", "OpenGL renderer: %s | %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    glGenTextures(1, &gl_color); glBindTexture(GL_TEXTURE_2D, gl_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SW, SH, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenRenderbuffers(1, &gl_depth); glBindRenderbuffer(GL_RENDERBUFFER, gl_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, SW, SH);
    glGenFramebuffers(1, &gl_fbo); glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, gl_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { rt_log("gl", "framebuffer incomplete"); return 0; }
    glViewport(0, 0, SW, SH);
    rt_log("gl", "internal resolution %dx%d (%dx)", SW, SH, gl_scale);
    glClearColor(0, 0, 0, 0); glClearDepth(1.0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_CLAMP); glEnable(GL_SCISSOR_TEST); glFrontFace(GL_CW);
    glGenVertexArrays(1, &gl_vao); glBindVertexArray(gl_vao);
    glGenBuffers(1, &gl_vbo); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
    glBufferData(GL_ARRAY_BUFFER, gl_vbo_size, NULL, GL_STREAM_DRAW);
    glGenBuffers(1, &gl_ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, gl_ibo_size, NULL, GL_STREAM_DRAW);
    for (int i = 3; i <= 10; i++) glVertexAttrib4f(i, 0, 0, 1, 1);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);
    static const GLint wrap[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
    for (int s = 0; s < 3; s++) for (int t = 0; t < 3; t++) for (int l = 0; l < 2; l++) {
        GLuint sm; glGenSamplers(1, &sm);
        glSamplerParameteri(sm, GL_TEXTURE_WRAP_S, wrap[s]); glSamplerParameteri(sm, GL_TEXTURE_WRAP_T, wrap[t]);
        glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, l ? GL_LINEAR : GL_NEAREST); glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, l ? GL_LINEAR : GL_NEAREST);
        gl_samplers[s][t][l] = sm;
    }
    for (int s = 0; s < 3; s++) for (int t = 0; t < 3; t++) {        /* custom textures: trilinear */
        GLuint sm; glGenSamplers(1, &sm);
        glSamplerParameteri(sm, GL_TEXTURE_WRAP_S, wrap[s]); glSamplerParameteri(sm, GL_TEXTURE_WRAP_T, wrap[t]);
        glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl_samplers_hd[s][t] = sm;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl_filter = rt_setting("texture_filter", 1); if (gl_filter < 0 || gl_filter > 5) gl_filter = 1;
    gl_apply_filter();
    return 1;
}
static void gl_apply_filter(void) {
    static const float aniso[6] = {1, 1, 2, 4, 8, 16};
    float an = aniso[gl_filter];
    for (int s = 0; s < 3; s++) for (int t = 0; t < 3; t++) {
        for (int l = 0; l < 2; l++) {
            GLuint sm = gl_samplers[s][t][l];
            int lin = gl_filter != 0 && l;
            glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, !lin ? GL_NEAREST : gl_filter >= 2 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, lin ? GL_LINEAR : GL_NEAREST);
            glSamplerParameterf(sm, 0x84FE, lin ? an : 1.0f);         /* GL_TEXTURE_MAX_ANISOTROPY */
        }
        GLuint sm = gl_samplers_hd[s][t];
        glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, gl_filter == 0 ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
        glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, gl_filter == 0 ? GL_NEAREST : GL_LINEAR);
        glSamplerParameterf(sm, 0x84FE, an);
    }
    while (glGetError()) {}                                   /* (anisotropy unsupported: ignored) */
}
static void gl_set_filter(int mode) {
    if (mode < 0 || mode > 5 || mode == gl_filter) return;
    gl_flush();
    int remip = (mode >= 2) != (gl_filter >= 2);
    gl_filter = mode; gl_apply_filter();
    if (remip) for (int i = 0; i < TEXCACHE; i++) gltex[i].ver = 0;   /* re-upload with / without mipmaps */
    rt_log("gl", "texture filtering %d", mode);
}

/* ------------------------------------------------------------------ state */
static const GLenum gl_cmpfn[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
static GLenum blend_factor(int f, int src, int has_alpha) {
    switch (f) {
    case 0: return GL_ZERO; case 1: return GL_ONE;
    case 2: return src ? GL_DST_COLOR : GL_SRC_COLOR; case 3: return src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 4: return GL_SRC_ALPHA; case 5: return GL_ONE_MINUS_SRC_ALPHA;
    case 6: return has_alpha ? GL_DST_ALPHA : GL_ONE; default: return has_alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ZERO;
    }
}
static struct { int valid; int blend, eq; GLenum sf, df, sfa, dfa; int bconst; int cmask; int zen, zfn, zupd; int cull; int sc[4]; } gls;
static int dbg_nodepth = -1, dbg_nocull, dbg_noblend, dbg_nobatch;

/* raster state (blend, masks, depth, cull, scissor) from the BP registers */
static void gl_state_raster(glstate *st) {
    uint32_t cm = bp(0x41), zm = bp(0x40);
    if (dbg_nodepth < 0) { dbg_nodepth = getenv("RT_GLNODEPTH") != NULL; dbg_nocull = getenv("RT_GLNOCULL") != NULL; dbg_noblend = getenv("RT_GLNOBLEND") != NULL;
                           dbg_nobatch = getenv("RT_GLNOBATCH") != NULL; }
    if (dbg_nodepth) zm &= ~1u; if (dbg_noblend) cm &= ~1u;
    int has_alpha = (bp(0x43) & 7) == 1;
    int blend = cm & 1, bsub = (cm >> 11) & 1, sf = (cm >> 8) & 7, df = (cm >> 5) & 7;
    int dstconst_en = (bp(0x42) >> 8) & 1, dstconst = bp(0x42) & 255;
    int en = 0, eq = GL_FUNC_ADD; GLenum s = GL_ONE, d = GL_ZERO, sa, da;
    if (blend && bsub) { en = 1; eq = GL_FUNC_REVERSE_SUBTRACT; s = GL_ONE; d = GL_ONE; }
    else if (blend) { en = 1; s = blend_factor(sf, 1, has_alpha); d = blend_factor(df, 0, has_alpha); }
    sa = s; da = d;
    if (dstconst_en) { en = 1; sa = GL_CONSTANT_ALPHA; da = GL_ZERO; if (eq == GL_FUNC_REVERSE_SUBTRACT) { eq = GL_FUNC_ADD; s = GL_ONE; d = GL_ZERO; } }
    st->blend = en;
    if (en) { st->eq = eq; st->sf = s; st->df = d; st->sfa = sa; st->dfa = da; }
    st->dstconst_en = dstconst_en; if (dstconst_en) st->bconst = dstconst;
    st->cmask = ((cm >> 3) & 1) | (((cm >> 4) & 1) << 1);
    st->zen = zm & 1; st->zfn = st->zen ? (zm >> 1) & 7 : 0; st->zupd = (zm >> 4) & 1;
    /* GX "front" = clockwise as seen on screen; the y flip keeps the picture upright, so it is clockwise in GL too */
    st->cull = dbg_nocull ? 0 : (bp(0x00) >> 14) & 3;
    setup_scissor();
    st->sc[0] = scissor_l; st->sc[1] = EFB_H - 1 - scissor_b; st->sc[2] = scissor_r - scissor_l + 1; st->sc[3] = scissor_b - scissor_t + 1;
    if (st->sc[2] < 0) st->sc[2] = 0; if (st->sc[3] < 0) st->sc[3] = 0;
}

static void gl_apply_raster(const glstate *st) {
    if (!gls.valid || st->blend != gls.blend) { if (st->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND); gls.blend = st->blend; }
    if (st->blend && (!gls.valid || st->eq != gls.eq || st->sf != gls.sf || st->df != gls.df || st->sfa != gls.sfa || st->dfa != gls.dfa)) {
        glBlendEquation(st->eq); glBlendFuncSeparate(st->sf, st->df, st->sfa, st->dfa);
        gls.eq = st->eq; gls.sf = st->sf; gls.df = st->df; gls.sfa = st->sfa; gls.dfa = st->dfa;
    }
    if (st->dstconst_en && (!gls.valid || gls.bconst != st->bconst)) { glBlendColor(0, 0, 0, st->bconst / 255.0f); gls.bconst = st->bconst; }
    if (!gls.valid || st->cmask != gls.cmask) { glColorMask(st->cmask & 1, st->cmask & 1, st->cmask & 1, (st->cmask >> 1) & 1); gls.cmask = st->cmask; }
    if (!gls.valid || st->zen != gls.zen) { if (st->zen) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); gls.zen = st->zen; }
    if (st->zen && (!gls.valid || st->zfn != gls.zfn)) { glDepthFunc(gl_cmpfn[st->zfn]); gls.zfn = st->zfn; }
    if (!gls.valid || st->zupd != gls.zupd) { glDepthMask(st->zupd); gls.zupd = st->zupd; }
    if (!gls.valid || st->cull != gls.cull) {
        if (!st->cull) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glCullFace(st->cull == 1 ? GL_BACK : st->cull == 2 ? GL_FRONT : GL_FRONT_AND_BACK); }
        gls.cull = st->cull;
    }
    if (!gls.valid || memcmp(st->sc, gls.sc, sizeof st->sc)) { glScissor(st->sc[0] * gl_scale, st->sc[1] * gl_scale, st->sc[2] * gl_scale, st->sc[3] * gl_scale); memcpy(gls.sc, st->sc, sizeof st->sc); }
    gls.valid = 1;
}

/* everything a draw needs, from the current BP state; returns 0 if the draw can't be rendered */
static int gl_build_state(glstate *st) {
    memset(st, 0, sizeof *st);
    uint32_t gen = bp(0x00);
    int nst = ((gen >> 10) & 15) + 1, ntex = gen & 15; if (ntex > 8) ntex = 8;
    tevkey k; memset(&k, 0, sizeof k);
    k.nst = (uint32_t)nst;
    for (int s = 0; s < nst; s++) {
        uint32_t tref = bp(0x28 + s / 2) >> ((s & 1) * 12);
        k.c[s] = bp(0xC0 + 2 * s) & 0xFFFFFF; k.a[s] = bp(0xC1 + 2 * s) & 0xFFFFFF;
        k.texen[s] = (tref >> 6) & 1; k.coord[s] = (tref >> 3) & 7; k.chan[s] = (tref >> 7) & 7;
        if ((int)k.coord[s] >= ntex) k.coord[s] = 0;
    }
    for (int i = 0; i < 8; i++) k.ksel[i] = bp(0xF6 + i) & 0xFFFFFF;
    k.acmp = bp(0xF3) & 0xFF0000;
    k.nst |= (uint32_t)ntex << 8;
    k.nind = (gen >> 16) & 7; if (k.nind > 4) k.nind = 4;
    if (k.nind) { k.iref = bp(0x27) & 0xFFFFFF; k.ss0 = bp(0x25) & 0xFFFF; k.ss1 = bp(0x26) & 0xFFFF; }
    for (int s = 0; s < nst; s++) k.ind[s] = bp(0x10 + s) & 0x1FFFFF;
    extern uint64_t g_perf_key_ns; uint64_t tk1 = rt_now_ns();
    glprog *pg = gl_program(&k);
    g_perf_key_ns += rt_now_ns() - tk1;
    if (!pg) return 0;
    st->pg = pg; st->ntex = ntex;
    gl_state_raster(st);
    gluni *u = &st->u;
    for (int i = 0; i < 4; i++) {
        u->regs[i * 4] = tev_reg[i].r; u->regs[i * 4 + 1] = tev_reg[i].g; u->regs[i * 4 + 2] = tev_reg[i].b; u->regs[i * 4 + 3] = tev_reg[i].a;
        u->kon[i * 4] = tev_konst[i].r; u->kon[i * 4 + 1] = tev_konst[i].g; u->kon[i * 4 + 2] = tev_konst[i].b; u->kon[i * 4 + 3] = tev_konst[i].a;
    }
    u->aref[0] = (GLint)(bp(0xF3) & 255); u->aref[1] = (GLint)((bp(0xF3) >> 8) & 255);
    if (pg->u_cscale >= 0)
        for (int c = 0; c < 8; c++) { u->cs[c * 2] = (GLint)(((bp(0x30 + 2 * c) & 0xFFFF) + 1) * 128); u->cs[c * 2 + 1] = (GLint)(((bp(0x31 + 2 * c) & 0xFFFF) + 1) * 128); }
    if (pg->u_indmtx >= 0)
        for (int m = 0; m < 3; m++) {
            uint32_t col[3] = {bp(0x06 + 3 * m), bp(0x07 + 3 * m), bp(0x08 + 3 * m)};
            int scale = (int)(((col[0] >> 22) & 3) | (((col[1] >> 22) & 3) << 2) | (((col[2] >> 22) & 3) << 4));
            for (int j = 0; j < 3; j++) {
                int ma = (int)(col[j] & 0x7FF), mb = (int)((col[j] >> 11) & 0x7FF);
                if (ma & 0x400) ma -= 0x800; if (mb & 0x400) mb -= 0x800;
                u->im[(2 * m) * 4 + j] = ma; u->im[(2 * m + 1) * 4 + j] = mb;
            }
            u->im[(2 * m) * 4 + 3] = u->im[(2 * m + 1) * 4 + 3] = 17 - scale;
        }
    for (int i = 0; i < 8; i++) u->inddim[i] = 1;
    for (int i = 0; i < 32; i++) u->mapdim[i] = 1;
    /* indirect maps on units 8.., stage textures on unit s */
    for (int i = 0; i < (int)k.nind; i++) {
        int map = (k.iref >> (6 * i)) & 7;
        int g = map >> 2, j = map & 3; uint32_t mode0 = bp((g ? 0xA0 : 0x80) + j);
        int ws = mode0 & 3, wt = (mode0 >> 2) & 3, lin = (mode0 >> 4) & 1; if (ws > 2) ws = 0; if (wt > 2) wt = 0;
        uint32_t tw, th; GLuint gt = gc_lookup(map, &tw, &th);
        int hd = 0;
        if (!gt) { texent *t = get_texture(map); gt = gl_texture(t, &hd); tw = t->w; th = t->h; }
        st->tex[8 + i] = gt; st->smp[8 + i] = hd ? gl_samplers_hd[ws][wt] : gl_samplers[ws][wt][lin];
        u->inddim[i * 2] = (GLint)tw; u->inddim[i * 2 + 1] = (GLint)th;
    }
    for (int s = 0; s < nst; s++) if (k.texen[s]) {
        uint32_t tref = bp(0x28 + s / 2) >> ((s & 1) * 12);
        int map = tref & 7;
        int g = map >> 2, i = map & 3; uint32_t mode0 = bp((g ? 0xA0 : 0x80) + i);
        int ws = mode0 & 3, wt = (mode0 >> 2) & 3, lin = (mode0 >> 4) & 1;
        if (ws > 2) ws = 0; if (wt > 2) wt = 0;
        uint32_t tw, th; GLuint gt = gc_lookup(map, &tw, &th);
        int hd = 0;
        if (!gt) { texent *t = get_texture(map); gt = gl_texture(t, &hd); tw = t->w; th = t->h; }
        st->tex[s] = gt; st->smp[s] = hd ? gl_samplers_hd[ws][wt] : gl_samplers[ws][wt][lin];
        u->mapdim[s * 2] = (GLint)tw; u->mapdim[s * 2 + 1] = (GLint)th;
    }
    return 1;
}

/* ------------------------------------------------------------------ batch submission */
static void gl_flush(void) {
    if (!bpending) return;
    uint64_t t0 = rt_now_ns();
    bpending = 0;
    const glstate *st = &gl_bst;
    glprog *pg = st->pg;
    gl_apply_raster(st);
    if (gl_bound_prog != pg->prog) { glUseProgram(pg->prog); gl_bound_prog = pg->prog; }
    const gluni *u = &st->u, *c = &pg->ucache; int ok = pg->ucache_ok;
#define UPD(field, loc, fn, n) if (loc >= 0 && (!ok || memcmp(u->field, c->field, sizeof u->field))) fn(loc, n, u->field)
    UPD(regs, pg->u_reg, glUniform4iv, 4);
    UPD(kon, pg->u_konst, glUniform4iv, 4);
    UPD(aref, pg->u_aref, glUniform2iv, 1);
    UPD(cs, pg->u_cscale, glUniform2iv, 8);
    UPD(im, pg->u_indmtx, glUniform4iv, 6);
    UPD(inddim, pg->u_inddim, glUniform2iv, 4);
    UPD(mapdim, pg->u_mapdim, glUniform2iv, 16);
#undef UPD
    pg->ucache = *u; pg->ucache_ok = 1;
    for (int unit = 0; unit < 12; unit++) {
        if (!st->tex[unit]) continue;
        if (bound_tex[unit] != st->tex[unit]) { gl_unit(unit); glBindTexture(GL_TEXTURE_2D, st->tex[unit]); bound_tex[unit] = st->tex[unit]; }
        if (bound_smp[unit] != st->smp[unit]) { glBindSampler(unit, st->smp[unit]); bound_smp[unit] = st->smp[unit]; }
    }
    /* upload vertices + indices (ring buffers, orphaned when full) */
    if (gl_vbo_off + bvlen > gl_vbo_size) { glBufferData(GL_ARRAY_BUFFER, gl_vbo_size, NULL, GL_STREAM_DRAW); gl_vbo_off = 0; }
    glBufferSubData(GL_ARRAY_BUFFER, gl_vbo_off, bvlen, bvbuf);
    size_t ilen = bilen * 4;
    if (gl_ibo_off + ilen > gl_ibo_size) { glBufferData(GL_ELEMENT_ARRAY_BUFFER, gl_ibo_size, NULL, GL_STREAM_DRAW); gl_ibo_off = 0; }
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, gl_ibo_off, ilen, bibuf);
    int ntex = st->ntex; GLsizei stride = (GLsizei)(24 + 12 * ntex);
    const char *base = (const char *)(uintptr_t)gl_vbo_off;
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, base);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, base + 16);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, base + 20);
    for (int tc = 0; tc < ntex; tc++) {
        if (tc >= enabled_tex) glEnableVertexAttribArray(3 + tc);
        glVertexAttribPointer(3 + tc, 3, GL_FLOAT, GL_FALSE, stride, base + 24 + 12 * tc);
    }
    if (enabled_tex > ntex) for (int tc = ntex; tc < enabled_tex; tc++) glDisableVertexAttribArray(3 + tc);
    enabled_tex = ntex;
    glDrawElements(GL_TRIANGLES, (GLsizei)bilen, GL_UNSIGNED_INT, (const void *)(uintptr_t)gl_ibo_off);
    gl_vbo_off += (bvlen + 255) & ~(size_t)255;
    gl_ibo_off += (ilen + 255) & ~(size_t)255;
    bvlen = 0; bilen = 0; bvcount = 0;
    g_perf_batches++;
    g_perf_flush_ns += rt_now_ns() - t0;
}

/* ------------------------------------------------------------------ draws */
static void gl_draw(int prim, int vat, uint32_t count, const uint8_t *data) {
    if (prim != 0x00 && prim != 0x08 && prim != 0x10 && prim != 0x18 && prim != 0x20) return;   /* lines/points: not yet */
    frame_draws++;
    /* state: rebuilt only when a register changed since the last draw */
    static uint32_t hd_seen;
    if (hd_seen != g_hd_ready) { hd_seen = g_hd_ready; gl_cur_ok = 0; }
    static uint32_t cur_seq, bst_seq;                   /* gl_cur rebuilds; the one the batch was started from */
    if (gl_cur_gen != g_gx_state_gen || !gl_cur_ok) {
        gl_cur_ok = gl_build_state(&gl_cur);
        gl_cur_gen = g_gx_state_gen; cur_seq++;
    }
    if (!gl_cur_ok) return;
    /* (unchanged since the batch began: no need to compare the whole state) */
    if (bpending && (dbg_nobatch || (cur_seq != bst_seq && memcmp(&gl_bst, &gl_cur, sizeof gl_cur)))) gl_flush();
    if (!bpending) gl_bst = gl_cur;
    bst_seq = cur_seq;
    /* triangle indices */
    size_t ntri = prim <= 0x08 ? (size_t)(count / 4) * 2 : prim == 0x10 ? count / 3 : count >= 3 ? count - 2 : 0;
    if (!ntri) return;
    int ntex = gl_cur.ntex;
    size_t stride = 24 + 12 * (size_t)ntex;
    if (bvlen + count * stride > (4u << 20) || bilen + ntri * 3 > (1u << 20)) { gl_flush(); gl_bst = gl_cur; }
    if (bvlen + count * stride > bvcap) { bvcap = (bvlen + count * stride) * 2; bvbuf = realloc(bvbuf, bvcap); }
    if (bilen + ntri * 3 > bicap) { bicap = (bilen + ntri * 3) * 2; bibuf = realloc(bibuf, bicap * 4); }
    uint32_t b0 = bvcount, *ix = bibuf + bilen;
    switch (prim) {
    case 0x00: case 0x08:
        for (uint32_t i = 0; i + 3 < count; i += 4) { uint32_t q = b0 + i; *ix++ = q; *ix++ = q + 1; *ix++ = q + 2; *ix++ = q; *ix++ = q + 2; *ix++ = q + 3; }
        break;
    case 0x10: for (uint32_t i = 0; i + 2 < count; i += 3) { *ix++ = b0 + i; *ix++ = b0 + i + 1; *ix++ = b0 + i + 2; } break;
    case 0x18: for (uint32_t i = 0; i + 2 < count; i++) {
                   if (i & 1) { *ix++ = b0 + i + 1; *ix++ = b0 + i; } else { *ix++ = b0 + i; *ix++ = b0 + i + 1; }
                   *ix++ = b0 + i + 2; } break;
    default:   for (uint32_t i = 1; i + 1 < count; i++) { *ix++ = b0; *ix++ = b0 + i; *ix++ = b0 + i + 1; } break;
    }
    bilen = (size_t)(ix - bibuf);
    /* vertices: CPU transform, then GX viewport -> GL clip space */
    static vtx *v; static uint32_t vcap;
    if (count > vcap) { vcap = count * 2; v = realloc(v, sizeof(vtx) * vcap); }
    const uint8_t *p = data;
    extern uint64_t g_perf_xform_ns; uint64_t tx0 = rt_now_ns();
    for (uint32_t i = 0; i < count; i++) { rawv r; decode_vertex(vat, &p, &r); transform_vertex(&r, &v[i]); }
    g_perf_xform_ns += rt_now_ns() - tx0;
    float sx = xf_f(0x101A), sy = xf_f(0x101B), sz = xf_f(0x101C), ox = xf_f(0x101D), oy = xf_f(0x101E), oz = xf_f(0x101F);
    float so_x = (float)((bp(0x59) & 0x3FF) * 2) - 342.0f, so_y = (float)(((bp(0x59) >> 10) & 0x3FF) * 2) - 342.0f;
    float bx = ox - 342.0f - so_x, by = oy - 342.0f - so_y;
    uint8_t *o = bvbuf + bvlen;
    for (uint32_t n = 0; n < count; n++, o += stride) {
        const vtx *s = &v[n];
        float *f = (float *)o;
        float wx = s->x * sx + bx * s->w, wy = s->y * sy + by * s->w, wz = s->z * sz + oz * s->w;
        f[0] = wx * (2.0f / EFB_W) - s->w;
        f[1] = s->w - wy * (2.0f / EFB_H);
        f[2] = wz * (2.0f / 16777215.0f) - s->w;
        f[3] = s->w;
        uint8_t *c = o + 16;
        for (int ch = 0; ch < 2; ch++) for (int q = 0; q < 4; q++) { float cv = s->col[ch][q]; c[ch * 4 + q] = cv <= 0 ? 0 : cv >= 255 ? 255 : (uint8_t)cv; }
        float *t = (float *)(o + 24);
        for (int tc = 0; tc < ntex; tc++) { t[tc * 3] = s->tex[tc][0]; t[tc * 3 + 1] = s->tex[tc][1]; t[tc * 3 + 2] = s->tex[tc][2]; }
    }
    bvlen += count * stride; bvcount += count;
    bpending = 1;
    /* RT_PIXTRACE="frame,x,y": log every draw that changes EFB pixel (x,y) in that frame */
    static int pt_init, pt_frame = -1, pt_x, pt_y; static uint32_t pt_last;
    if (!pt_init) { pt_init = 1; const char *e = getenv("RT_PIXTRACE"); if (e) sscanf(e, "%d,%d,%d", &pt_frame, &pt_x, &pt_y); }
    if (pt_frame >= 0 && g_frames_presented == (uint64_t)pt_frame) {
        gl_flush();
        uint32_t px; glReadPixels(pt_x * gl_scale, (EFB_H - 1 - pt_y) * gl_scale, 1, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &px);
        if (px != pt_last) {
            int nst = ((bp(0x00) >> 10) & 15) + 1;
            rt_log("pix", "draw %u: %08X -> %08X prim %02X n %u vat %d vcd %08X/%08X VAT %08X/%08X/%08X", frame_draws, pt_last, px, prim, count, vat,
                   gx.cp_vcd_lo, gx.cp_vcd_hi, gx.cp_vat_a[vat], gx.cp_vat_b[vat], gx.cp_vat_c[vat]);
            rt_log("pix", "  gen %06X nchan %u chan0 c/a %08X/%08X chan1 %08X/%08X mat0 %08X amb0 %08X mat1 %08X amb1 %08X",
                   bp(0x00), xf_r(0x09), xf_r(0x0E), xf_r(0x10), xf_r(0x0F), xf_r(0x11), xf_r(0x0C), xf_r(0x0A), xf_r(0x0D), xf_r(0x0B));
            for (int t = 0; t < ntex; t++) rt_log("pix", "  texgen %d: info %08X post %08X (dual %u)", t, xf_r(0x40 + t), xf_r(0x50 + t), xf_r(0x12));
            for (int s2 = 0; s2 < nst; s2++) {
                uint32_t tref = bp(0x28 + s2 / 2) >> ((s2 & 1) * 12);
                rt_log("pix", "  stage %d: tref map %u coord %u texen %u chan %u | c %06X a %06X ksel %06X", s2, tref & 7, (tref >> 3) & 7, (tref >> 6) & 1, (tref >> 7) & 7,
                       bp(0xC0 + 2 * s2), bp(0xC1 + 2 * s2), bp(0xF6 + s2 / 2));
            }
            for (int i = 0; i < 3 && i < (int)count; i++)
                rt_log("pix", "  v%d col0 %.0f %.0f %.0f %.0f col1 %.0f %.0f %.0f %.0f tex0 %g %g %g", i, v[i].col[0][0], v[i].col[0][1], v[i].col[0][2], v[i].col[0][3],
                       v[i].col[1][0], v[i].col[1][1], v[i].col[1][2], v[i].col[1][3], v[i].tex[0][0], v[i].tex[0][1], v[i].tex[0][2]);
            pt_last = px;
        }
    }
}

/* read an EFB rectangle into efb_rb (EFB layout, rows top-down) */
static const uint32_t *gl_read_efb(int sx, int sy, int w, int h) {
    gl_flush();
    static uint32_t *tmp; static size_t tcap;
    int S = gl_scale; size_t sw = (size_t)w * S, sh = (size_t)h * S;
    if (sw * sh > tcap) { tcap = sw * sh; tmp = realloc(tmp, tcap * 4); }
    glReadPixels(sx * S, (EFB_H - (sy + h)) * S, (GLsizei)sw, (GLsizei)sh, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, tmp);
    for (int y = 0; y < h; y++) {                 /* one sample per EFB pixel (rows are bottom-up in GL) */
        const uint32_t *row = &tmp[(sh - 1 - (size_t)y * S - S / 2) * sw];
        for (int x = 0; x < w; x++) efb_rb[(sy + y) * EFB_W + sx + x] = row[(size_t)x * S + S / 2];
    }
    return efb_rb;
}
static void gl_clear(int sx, int sy, int w, int h) {
    gl_flush();
    uint32_t ar = bp(0x4F), gb = bp(0x50), z = bp(0x51) & 0xFFFFFF;
    uint32_t cm = bp(0x41), zm = bp(0x40);
    GLbitfield m = 0;
    int cmask = (cm >> 3) & 1, amask = (cm >> 4) & 1;
    if (cmask || amask) {
        glColorMask(cmask, cmask, cmask, amask);
        glClearColor((ar & 255) / 255.0f, ((gb >> 8) & 255) / 255.0f, (gb & 255) / 255.0f, ((ar >> 8) & 255) / 255.0f);
        m |= GL_COLOR_BUFFER_BIT;
    }
    if (zm & 16) { glDepthMask(1); glClearDepth(z / 16777215.0); m |= GL_DEPTH_BUFFER_BIT; }
    if (!m) return;
    glScissor(sx * gl_scale, (EFB_H - (sy + h)) * gl_scale, w * gl_scale, h * gl_scale);
    glClear(m);
    gls.valid = 0;                            /* masks/scissor changed */
}

/* ------------------------------------------------------------------ EFB copies to textures, kept on the GPU
 * An EFB->texture copy renders the copied rectangle, converted to the destination format
 * (quantized exactly like the CPU encoder + texture decoder would), straight into a GL texture.
 * Nothing is read back or written to RAM except a small tag at the destination; draws that later
 * sample that address in the matching format/size use the GL texture directly. If the address
 * is read any other way (different format/size) the copy is "materialized" into RAM then, and if
 * the tag was overwritten (memory reused) the copy is dropped. */
typedef struct { uint32_t addr, size, w, h; int nat; GLuint tex, fbo; uint32_t tag; } gpucopy;
#define NGC 128
static gpucopy gcs[NGC]; static int ngc;
static GLuint gc_fbo, gc_prog; static GLint gc_u_rect, gc_u_fmt, gc_u_h; static uint32_t gc_seq;
static int gc_mode = -1;                  /* 1 = GPU copies on (RT_GLCPUCOPY=1 turns them off) */
static void write_tex_texel(uint8_t *dst, int fmt, int x, int y, int w, uint32_t c, int intensity);

static int gl_gpucopy_on(void) {
    if (gc_mode < 0) {
        gc_mode = !getenv("RT_GLCPUCOPY");
        if (gc_mode) {
            static const char *vs = "#version 330 core\nvoid main() { gl_Position = vec4(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1), 0.0, 1.0); }\n";
            static const char *fs =
                "#version 330 core\nuniform sampler2D uE; uniform ivec4 uRect; uniform int uFmt; uniform int uH; out vec4 o;\n"
                "ivec4 E(int x, int y) { return ivec4(texelFetch(uE, ivec2(x, uH - 1 - y), 0) * 255.0 + 0.5); }\n"
                "void main() {\n"
                "  ivec2 d = ivec2(gl_FragCoord.xy); int sx = uRect.x, sy = uRect.y; ivec4 c;   // (scaled EFB coordinates)\n"
                "  if (uRect.z != 0) c = (E(sx + 2 * d.x, sy + 2 * d.y) + E(sx + 2 * d.x + 1, sy + 2 * d.y) + E(sx + 2 * d.x, sy + 2 * d.y + 1) + E(sx + 2 * d.x + 1, sy + 2 * d.y + 1)) / 4;\n"
                "  else c = E(sx + d.x, sy + d.y);\n"
                "  int r = c.r, g = c.g, b = c.b, a = c.a;\n"
                "  int lum = uRect.w != 0 ? (r * 66 + g * 129 + b * 25 + 128) / 256 + 16 : r;\n"
                "  ivec4 res = c;\n"
                "  if (uFmt == 0) res = ivec4((lum >> 4) * 17);\n"
                "  else if (uFmt == 1) res = ivec4(lum);\n"
                "  else if (uFmt == 2) { int i = (lum >> 4) * 17; res = ivec4(i, i, i, (a >> 4) * 17); }\n"
                "  else if (uFmt == 3) res = ivec4(lum, lum, lum, a);\n"
                "  else if (uFmt == 4) { int R = r >> 3, G = g >> 2, B = b >> 3; res = ivec4((R << 3) | (R >> 2), (G << 2) | (G >> 4), (B << 3) | (B >> 2), 255); }\n"
                "  else if (uFmt == 5) {\n"
                "    if (a >= 224) { int R = r >> 3, G = g >> 3, B = b >> 3; res = ivec4((R << 3) | (R >> 2), (G << 3) | (G >> 2), (B << 3) | (B >> 2), 255); }\n"
                "    else { int A = a >> 5; res = ivec4((r >> 4) * 17, (g >> 4) * 17, (b >> 4) * 17, (A << 5) | (A << 2) | (A >> 1)); } }\n"
                "  else if (uFmt == 7) res = ivec4(a);\n"
                "  else if (uFmt == 8) res = ivec4(r);\n"
                "  else if (uFmt == 9) res = ivec4(g);\n"
                "  else if (uFmt == 10) res = ivec4(b);\n"
                "  else if (uFmt == 11) res = ivec4(r, r, r, g);\n"
                "  else if (uFmt == 12) res = ivec4(g, g, g, b);\n"
                "  o = vec4(res) / 255.0;\n}\n";
            GLuint v = gl_compile(GL_VERTEX_SHADER, vs), f = gl_compile(GL_FRAGMENT_SHADER, fs);
            gc_prog = glCreateProgram(); glAttachShader(gc_prog, v); glAttachShader(gc_prog, f); glLinkProgram(gc_prog);
            GLint ok = 0; glGetProgramiv(gc_prog, GL_LINK_STATUS, &ok);
            if (!ok) { rt_log("gl", "copy shader failed; EFB copies go through the CPU"); gc_mode = 0; return 0; }
            glUseProgram(gc_prog); gl_bound_prog = gc_prog;
            glUniform1i(glGetUniformLocation(gc_prog, "uE"), 14);
            gc_u_rect = glGetUniformLocation(gc_prog, "uRect"); gc_u_fmt = glGetUniformLocation(gc_prog, "uFmt");
            gc_u_h = glGetUniformLocation(gc_prog, "uH"); glUniform1i(gc_u_h, SH);
            glGenFramebuffers(1, &gc_fbo);
        }
    }
    return gc_mode;
}

static int natural_fmt(int fmt) { return fmt <= 6 ? fmt : fmt <= 10 ? 1 : 3; }
static void gc_remove(int i) { glDeleteFramebuffers(1, &gcs[i].fbo); glDeleteTextures(1, &gcs[i].tex); for (int u = 0; u < 16; u++) if (bound_tex[u] == gcs[i].tex) bound_tex[u] = 0; gcs[i] = gcs[--ngc]; }
static int gc_tag_ok(const gpucopy *e) {
    const uint8_t *m = g_base + (e->addr & 0x3FFFFFFF);
    return m[0] == 'G' && m[1] == 'L' && m[2] == 'C' && m[3] == 'P' && (((uint32_t)m[4] << 24) | (m[5] << 16) | (m[6] << 8) | m[7]) == e->tag;
}

static void gl_copy_to_texture(int sx, int sy, int w, int h, uint32_t dest, int fmt, int intensity, int half) {
    gl_flush();
    int dw = half ? w / 2 : w, dh = half ? h / 2 : h, nat = natural_fmt(fmt);
    if (dw <= 0 || dh <= 0) return;
    static const int bw[7] = {8, 8, 8, 4, 4, 4, 4}, bh[7] = {8, 4, 4, 4, 4, 4, 4};
    uint32_t size = (uint32_t)(((dw + bw[nat] - 1) / bw[nat]) * ((dh + bh[nat] - 1) / bh[nat]) * (nat == 6 ? 64 : 32));
    gpucopy *e = NULL;
    for (int i = 0; i < ngc; i++) {
        gpucopy *o = &gcs[i];
        if (o->addr == dest && o->w == (uint32_t)dw && o->h == (uint32_t)dh) { e = o; continue; }
        if (o->addr < dest + size && dest < o->addr + o->size) { gc_remove(i); i--; }   /* overwritten */
    }
    if (!e) {
        if (ngc == NGC) gc_remove(0);
        e = &gcs[ngc++]; memset(e, 0, sizeof *e);
        glGenTextures(1, &e->tex);
        gl_unit(15); glBindTexture(GL_TEXTURE_2D, e->tex); bound_tex[15] = e->tex;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dw * gl_scale, dh * gl_scale, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);   /* at internal resolution */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);       /* (no mipmaps: complete under any filter) */
        glGenFramebuffers(1, &e->fbo); glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);   /* one FBO per copy target: no re-attaching */
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->tex, 0);
    }
    e->addr = dest; e->size = size; e->w = (uint32_t)dw; e->h = (uint32_t)dh; e->nat = nat;
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
    glViewport(0, 0, dw * gl_scale, dh * gl_scale); glScissor(0, 0, dw * gl_scale, dh * gl_scale);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glColorMask(1, 1, 1, 1);
    if (gl_bound_prog != gc_prog) { glUseProgram(gc_prog); gl_bound_prog = gc_prog; }
    GLint rect[4] = {sx * gl_scale, sy * gl_scale, half, intensity}; glUniform4iv(gc_u_rect, 1, rect); glUniform1i(gc_u_fmt, fmt);
    if (bound_tex[14] != gl_color) { gl_unit(14); glBindTexture(GL_TEXTURE_2D, gl_color); bound_tex[14] = gl_color; }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);
    glViewport(0, 0, SW, SH);
    gls.valid = 0;
    /* tag the destination so reuse of that memory is noticed */
    uint8_t *m = g_base + (dest & 0x3FFFFFFF); e->tag = ++gc_seq;
    m[0] = 'G'; m[1] = 'L'; m[2] = 'C'; m[3] = 'P'; m[4] = e->tag >> 24; m[5] = e->tag >> 16; m[6] = e->tag >> 8; m[7] = e->tag;
}

/* An EFB copy into part of a texture we already have on the GPU (animated tiles: each frame of
   an animation is drawn into the EFB and copied into its place in the tileset, with the
   tileset's row stride): blit it straight into that GL texture. No read-back, so no stall.
   RAM keeps the tileset's original bytes, which the band check then sees as unchanged.
   Returns 0 when there is no such texture (the caller copies through RAM). */
static int gl_copy_into_texture(int sx, int sy, int w, int h, uint32_t dest, int fmt, int stride_bytes) {
    static const int cbw[16] = {8, 8, 8, 4, 4, 4, 4, 8, 8, 8, 4, 4, 4, 0, 0, 0}, cbh[16] = {8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0};
    int tw = cbw[fmt & 15], th = cbh[fmt & 15], bpb = fmt == 6 ? 64 : 32;
    if (!tw || fmt > 6) return 0;
    uint32_t d = dest & 0x3FFFFFFF;
    for (int i = 0; i < TEXCACHE; i++) {
        texent *t = &texcache[i];
        if (!t->px || t->hd || t->fmt != (uint32_t)fmt) continue;
        uint32_t a = t->addr & 0x3FFFFFFF;
        if (d < a || d >= a + tex_bytes(t->w, t->h, t->fmt)) continue;
        int bpr = (int)((t->w + tw - 1) / tw);
        if (bpr * bpb != stride_bytes) continue;
        gltexslot *g = &gltex[i];
        if (!g->tex || g->ver != t->ver) return 0;            /* (not uploaded yet: through RAM) */
        uint32_t blk = (d - a) / bpb; int x = (int)(blk % bpr) * tw, y = (int)(blk / bpr) * th;
        if (x + w > (int)t->w || y + h > (int)t->h) return 0;
        gl_flush();
        static GLuint fbo; if (!fbo) glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->tex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gl_fbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(sx * gl_scale, (EFB_H - (sy + h)) * gl_scale, (sx + w) * gl_scale, (EFB_H - sy) * gl_scale,
                          x, y + h, x + w, y, GL_COLOR_BUFFER_BIT, GL_LINEAR);   /* (rows flipped: texture row 0 = top) */
        glEnable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);
        if (gl_filter >= 2) { gl_unit(15); glBindTexture(GL_TEXTURE_2D, g->tex); bound_tex[15] = g->tex; glGenerateMipmap(GL_TEXTURE_2D); }
        gls.valid = 0;
        return 1;
    }
    return 0;
}

/* write a GPU copy into RAM in its format (when it's read in a way the GL texture can't serve) */
static void gc_materialize(int i) {
    gpucopy *e = &gcs[i];
    static uint32_t *px; static size_t cap;
    int S = gl_scale; size_t W = (size_t)e->w * S;
    if (W * e->h * S > cap) { cap = W * e->h * S; px = realloc(px, cap * 4); }
    gl_flush();
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
    glReadPixels(0, 0, (GLsizei)W, (GLsizei)(e->h * S), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);
    uint8_t *dst = g_base + (e->addr & 0x3FFFFFFF);
    for (uint32_t y = 0; y < e->h; y++) for (uint32_t x = 0; x < e->w; x++) write_tex_texel(dst, e->nat, x, y, e->w, px[((size_t)y * S + S / 2) * W + (size_t)x * S + S / 2], 0);
    for (int k = 0; k < TEXCACHE; k++) if (texcache[k].addr == e->addr) texcache[k].addr = 0xFFFFFFFF;
    static int logged; if (logged++ < 8) rt_log("gl", "EFB copy at %08X read back to RAM (used as a different format/size)", e->addr);
    gc_remove(i);
}

/* GL texture for a texture map if it is a GPU-side EFB copy in the matching format, else 0 */
static GLuint gc_lookup(int map, uint32_t *w, uint32_t *h) {
    if (!ngc) return 0;
    int g = map >> 2, i = map & 3;
    uint32_t img0 = bp((g ? 0xA8 : 0x88) + i), img3 = bp((g ? 0xB4 : 0x94) + i);
    uint32_t tw = (img0 & 0x3FF) + 1, th = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15, addr = (img3 & 0xFFFFFF) << 5;
    for (int k = 0; k < ngc; k++) {
        gpucopy *e = &gcs[k];
        if (addr < e->addr || addr >= e->addr + e->size) continue;
        if (!gc_tag_ok(e)) { gc_remove(k); return 0; }               /* memory reused for something else */
        if (addr == e->addr && fmt == (uint32_t)e->nat && tw == e->w && th == e->h) { *w = tw; *h = th; return e->tex; }
        gc_materialize(k); return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ presenting (EFB -> XFB copy)
 * When the platform renders into the game window, the copied rectangle is scaled straight from
 * the EFB framebuffer into the window and swapped: no readback. */
/* ------------------------------------------------------------------ overlay UI (in-game menus)
 * Flat rectangles and 8x8 bitmap text (public-domain font8x8) drawn on the window after the
 * game's picture, in a 640x480 virtual space mapped onto the picture. */
#include "font8x8_basic.h"
static GLuint ui_prog, ui_vao, ui_vbo, ui_tex;
static float ui_v[6 * 8 * 4096]; static int ui_nv;
static GLuint ui_texs[8]; static int ui_ntex = 1;           /* 0 = built-in font, 1.. = registered (game font) */
static struct { int first, count, tex; } ui_seg[256]; static int ui_nseg;
static int ui_px, ui_py, ui_pw, ui_ph, ui_cw, ui_ch;
static void ui_init(void) {
    static const char *vs = "#version 330 core\nlayout(location=0) in vec2 aP; layout(location=1) in vec2 aT; layout(location=2) in vec4 aC;\n"
                            "out vec2 vT; out vec4 vC; void main() { gl_Position = vec4(aP, 0.0, 1.0); vT = aT; vC = aC; }\n";
    static const char *fs = "#version 330 core\nin vec2 vT; in vec4 vC; uniform sampler2D uT; out vec4 o;\n"
                            "void main() { vec4 t = texture(uT, vT); o = vec4(vC.rgb * t.rgb, vC.a * t.a); }\n";
    GLuint v = gl_compile(GL_VERTEX_SHADER, vs), f = gl_compile(GL_FRAGMENT_SHADER, fs);
    ui_prog = glCreateProgram(); glAttachShader(ui_prog, v); glAttachShader(ui_prog, f); glLinkProgram(ui_prog);
    glUseProgram(ui_prog); gl_bound_prog = ui_prog; glUniform1i(glGetUniformLocation(ui_prog, "uT"), 0);
    static uint8_t px[72][128][4];                     /* 16x8 glyph cells + a solid strip at the bottom */
    memset(px, 0, sizeof px);
    for (int c = 0; c < 128; c++) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        if ((uint8_t)font8x8_basic[c][y] & (1 << x)) { uint8_t *p = px[(c / 16) * 8 + y][(c % 16) * 8 + x]; p[0] = p[1] = p[2] = p[3] = 255; }
    for (int y = 64; y < 72; y++) for (int x = 0; x < 128; x++) { uint8_t *p = px[y][x]; p[0] = p[1] = p[2] = p[3] = 255; }
    glGenTextures(1, &ui_tex); gl_unit(15); glBindTexture(GL_TEXTURE_2D, ui_tex); bound_tex[15] = ui_tex;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 128, 72, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glGenVertexArrays(1, &ui_vao); glBindVertexArray(ui_vao);
    glGenBuffers(1, &ui_vbo); glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof ui_v, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void *)8);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, (void *)16);
    glBindVertexArray(gl_vao); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
}
static void ui_begin(int px, int py, int pw, int ph, int cw, int ch) {
    if (rt_widescreen()) { int w43 = pw * 3 / 4; px += (pw - w43) / 2; pw = w43; }   /* menus keep 4:3 proportions in the middle */
    ui_px = px; ui_py = py; ui_pw = pw; ui_ph = ph; ui_cw = cw; ui_ch = ch; ui_nv = 0; ui_nseg = 0; }
static int ui_cur_tex;
static void ui_quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t c) {
    if (ui_nv + 6 > (int)(sizeof ui_v / sizeof *ui_v / 8)) return;
    if (!ui_nseg || ui_seg[ui_nseg - 1].tex != ui_cur_tex) {
        if (ui_nseg == 256) return;
        ui_seg[ui_nseg].first = ui_nv; ui_seg[ui_nseg].count = 0; ui_seg[ui_nseg].tex = ui_cur_tex; ui_nseg++;
    }
    ui_seg[ui_nseg - 1].count += 6;
    float X0 = ui_px + x / 640.0f * ui_pw, X1 = ui_px + (x + w) / 640.0f * ui_pw;
    float Y0 = ui_py + ui_ph - y / 480.0f * ui_ph, Y1 = ui_py + ui_ph - (y + h) / 480.0f * ui_ph;
    X0 = X0 / ui_cw * 2 - 1; X1 = X1 / ui_cw * 2 - 1; Y0 = Y0 / ui_ch * 2 - 1; Y1 = Y1 / ui_ch * 2 - 1;
    float r = (c >> 24) / 255.0f, g = ((c >> 16) & 255) / 255.0f, b = ((c >> 8) & 255) / 255.0f, a = (c & 255) / 255.0f;
    const float q[6][4] = {{X0, Y0, u0, v0}, {X1, Y0, u1, v0}, {X1, Y1, u1, v1}, {X0, Y0, u0, v0}, {X1, Y1, u1, v1}, {X0, Y1, u0, v1}};
    for (int i = 0; i < 6; i++) { float *o = &ui_v[ui_nv++ * 8]; o[0] = q[i][0]; o[1] = q[i][1]; o[2] = q[i][2]; o[3] = q[i][3]; o[4] = r; o[5] = g; o[6] = b; o[7] = a; }
}
void ui_rect(float x, float y, float w, float h, uint32_t rgba) { ui_cur_tex = 0; ui_quad(x, y, w, h, 4 / 128.0f, 66 / 72.0f, 5 / 128.0f, 67 / 72.0f, rgba); }
void ui_text(float x, float y, float scale, const char *s, uint32_t rgba) {
    ui_cur_tex = 0;
    for (; *s; s++, x += 8 * scale) {
        int c = (uint8_t)*s & 127; float u = (c % 16) * 8 / 128.0f, v = (c / 16) * 8 / 72.0f;
        if (c != ' ') ui_quad(x + scale, y + scale, 8 * scale, 8 * scale, u, v, u + 8 / 128.0f, v + 8 / 72.0f, 0x000000A0u);   /* shadow */
        ui_quad(x, y, 8 * scale, 8 * scale, u, v, u + 8 / 128.0f, v + 8 / 72.0f, rgba);
    }
}
/* textured quad from a registered texture (normalized uv) */
void ui_image(int tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t rgba) {
    ui_cur_tex = tex; ui_quad(x, y, w, h, u0, v0, u1, v1, rgba);
}
/* register a 0xAARRGGBB image for ui_image; returns its id (0 on failure) */
int ui_register_texture(const uint32_t *argb, int w, int h) {
    if (!gl_on || ui_ntex >= 8) return 0;              /* (software renderer: no GL; the menus aren't drawn there) */
    GLuint t; glGenTextures(1, &t); gl_unit(15); glBindTexture(GL_TEXTURE_2D, t); bound_tex[15] = t;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, argb);
    glGenerateMipmap(GL_TEXTURE_2D);                    /* text is drawn smaller than the 64px glyphs */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ui_texs[ui_ntex] = t;
    return ui_ntex++;
}
static void ui_end(void) {
    if (!ui_nv) return;
    if (!ui_prog) ui_init();
    glViewport(0, 0, ui_cw, ui_ch);
    glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glUseProgram(ui_prog); gl_bound_prog = ui_prog;
    gl_unit(0); glBindSampler(0, 0); bound_smp[0] = 0;
    ui_texs[0] = ui_tex;
    glBindVertexArray(ui_vao); glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof ui_v, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)ui_nv * 32, ui_v);
    for (int i = 0; i < ui_nseg; i++) {
        glBindTexture(GL_TEXTURE_2D, ui_texs[ui_seg[i].tex]);
        glDrawArrays(GL_TRIANGLES, ui_seg[i].first, ui_seg[i].count);
    }
    bound_tex[0] = ui_texs[ui_nseg ? ui_seg[ui_nseg - 1].tex : 0];
    glBindVertexArray(gl_vao); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
    glViewport(0, 0, SW, SH);
    gls.valid = 0; ui_nv = 0;
}

/* menus drawn into the EFB itself (when the frame is read back instead of shown by GL) */
static void gl_overlay_efb(int sx, int sy, int w, int h) {
    gl_flush();
    glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1);
    ui_begin(sx * gl_scale, (EFB_H - (sy + h)) * gl_scale, w * gl_scale, h * gl_scale, SW, SH); menu_draw(); ui_end();
    glEnable(GL_SCISSOR_TEST);
}

/* change the internal resolution while running (options menu) */
static void gl_set_scale(int s) {
    if (s < 1) s = 1; if (s > 8) s = 8;
    if (s == gl_scale) return;
    gl_flush();
    gl_scale = s;
    gl_unit(15); glBindTexture(GL_TEXTURE_2D, gl_color); bound_tex[15] = gl_color;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SW, SH, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glBindRenderbuffer(GL_RENDERBUFFER, gl_depth); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, SW, SH);
    glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo); glViewport(0, 0, SW, SH);
    while (ngc) gc_remove(0);                          /* EFB copy textures are sized by the scale */
    if (gc_prog) { glUseProgram(gc_prog); gl_bound_prog = gc_prog; glUniform1i(gc_u_h, SH); }
    gls.valid = 0; gl_cur_ok = 0;
    rt_log("gl", "internal resolution %dx%d (%dx)", SW, SH, gl_scale);
}

static int gl_present(int sx, int sy, int w, int h) {
    int px, py, pw, ph, cw, ch;
    if (!plat_gl_present_begin(&px, &py, &pw, &ph, &cw, &ch)) return 0;
    gl_flush();
    if (cw > 0 && ch > 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gl_fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1);
        if (pw != cw || ph != ch) { glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); }   /* bars */
        glBlitFramebuffer(sx * gl_scale, (EFB_H - (sy + h)) * gl_scale, (sx + w) * gl_scale, (EFB_H - sy) * gl_scale, px, py, px + pw, py + ph, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        ui_begin(px, py, pw, ph, cw, ch); menu_draw(); ui_end();     /* in-game menus on top */
        glEnable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);
        gls.valid = 0;
    }
    plat_gl_present_end();
    return 1;
}
