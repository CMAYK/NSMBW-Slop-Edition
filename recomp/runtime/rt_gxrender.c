/* rt_gxrender.c - software implementation of the Wii GPU ("Flipper/Hollywood" GX pipeline).
 *
 * Vertex stage : attribute decode (direct/indexed, all component formats), position/normal
 *                matrices, perspective/ortho projection, viewport, lighting (2 channels,
 *                8 lights), texgen (regular + post-transform/dual-tex).
 * Raster stage : clip against near plane, scissor, edge-function rasterizer with
 *                perspective-correct colors and texture coordinates.
 * Pixel stage  : textures (I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CI4/CI8/CI14X2/CMPR, wrap modes,
 *                bilinear), TEV (16 stages, swap tables, konst, compare modes), alpha test,
 *                depth test, blending/logic op, EFB copies to XFB (presented) and to textures.
 * Indirect texturing and fog are not implemented yet. */
#include "rt.h"
#include "rt_hdtex.h"
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "rt_gx.h"
#include <math.h>

#define EFB_W 640
#define EFB_H 528
static uint32_t efb[EFB_W * EFB_H];         /* 0xAARRGGBB */
static uint32_t zbuf[EFB_W * EFB_H];        /* 24-bit depth */
static uint8_t tmem[1 << 20];
static uint32_t present_buf[EFB_W * EFB_H];

static int gl_on;                       /* OpenGL backend active (rt_gxgl.inc) */
static int gl_cmp;                      /* RT_GLCOMPARE: run both renderers, software is authoritative */
static inline uint32_t bp(int r) { return gx.bp[r]; }
static inline float xf_f(uint32_t addr) { float f; uint32_t u = addr < 0x800 ? gx.xf_mem[addr] : gx.xf_regs[(addr - 0x1000) & 0xFF]; memcpy(&f, &u, 4); return f; }
static inline uint32_t xf_r(int r) { return gx.xf_regs[r & 0xFF]; }
static inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* ================================================================== textures */
typedef struct { uint32_t addr, fmt, w, h, tlut, tlutfmt; uint32_t *px; uint64_t stamp, ver, hash, epoch, dgen; struct hdtex *hd;
                 uint64_t *bandh; uint32_t nbands, upd_y0, upd_y1; uint64_t pver; } texent;   /* bands: see band_check */
static uint64_t tex_ver;
int g_copystats; uint32_t g_copydst[64];
#define TEXCACHE 512
static texent texcache[TEXCACHE];
static uint64_t tex_stamp = 1;
static void **pending_free; static int npending, cappending;
static void defer_free(void *p) {
    if (!p) return;
    if (npending == cappending) { cappending = cappending ? cappending * 2 : 256; pending_free = realloc(pending_free, sizeof(void *) * cappending); }
    pending_free[npending++] = p;
}

static inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }
static uint32_t c565(uint16_t v) { uint8_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31; return rgba((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255); }
static uint32_t c5a3(uint16_t v) {
    if (v & 0x8000) { uint8_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31; return rgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255); }
    uint8_t a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
    return rgba(r * 17, g * 17, b * 17, (a << 5) | (a << 2) | (a >> 1));
}
static uint32_t cia8(uint16_t v) { uint8_t i = v & 0xFF, a = v >> 8; return rgba(i, i, i, a); }
static uint32_t tlut_color(uint32_t tlut_tmem, uint32_t fmt, uint32_t idx) {
    uint32_t o = (tlut_tmem + idx * 2) & (sizeof tmem - 2);
    uint16_t v = (uint16_t)(tmem[o] << 8 | tmem[o + 1]);
    return fmt == 1 ? c565(v) : fmt == 2 ? c5a3(v) : cia8(v);
}

/* fast decoders for the common formats: one block at a time, no per-pixel format switch
   (new areas bring in large textures; decoding them used to cost milliseconds in one frame) */
static uint32_t c5a3_lut[65536], c565_lut[65536]; static int luts_ready;
static void build_luts(void) {
    for (uint32_t v = 0; v < 65536; v++) { c5a3_lut[v] = c5a3((uint16_t)v); c565_lut[v] = c565((uint16_t)v); }
    luts_ready = 1;
}
static int decode_rows_fast(texent *t, uint32_t by0, uint32_t by1) {
    uint32_t w = t->w, h = t->h, fmt = t->fmt;
    int tw, th;
    switch (fmt) { case 0: tw = 8; th = 8; break; case 1: case 2: tw = 8; th = 4; break; case 3: case 4: case 5: case 6: tw = 4; th = 4; break; case 14: tw = 8; th = 8; break; default: return 0; }
    if (!luts_ready) build_luts();
    uint32_t xb = (w + tw - 1) / tw, bsz = fmt == 6 ? 64 : 32;
    const uint8_t *src = g_base + (t->addr & 0x3FFFFFFF) + (size_t)by0 * xb * bsz;
    uint32_t *out = t->px;
    uint32_t tmp[64];
    for (uint32_t by = by0; by < by1; by++) for (uint32_t bx = 0; bx < xb; bx++, src += bsz) {
        switch (fmt) {
        case 0: for (int i = 0; i < 32; i++) { uint8_t b = src[i]; uint32_t hi = (b >> 4) * 17u, lo = (b & 15) * 17u;
                    tmp[i * 2] = hi * 0x01010101u; tmp[i * 2 + 1] = lo * 0x01010101u; } break;
        case 1: for (int i = 0; i < 32; i++) tmp[i] = src[i] * 0x01010101u; break;
        case 2: for (int i = 0; i < 32; i++) { uint8_t b = src[i]; uint32_t in = (b & 15) * 17u, a = (b >> 4) * 17u; tmp[i] = a << 24 | in * 0x010101u; } break;
        case 3: for (int i = 0; i < 16; i++) tmp[i] = (uint32_t)src[i * 2] << 24 | src[i * 2 + 1] * 0x010101u; break;
        case 4: for (int i = 0; i < 16; i++) tmp[i] = c565_lut[src[i * 2] << 8 | src[i * 2 + 1]]; break;
        case 5: for (int i = 0; i < 16; i++) tmp[i] = c5a3_lut[src[i * 2] << 8 | src[i * 2 + 1]]; break;
        case 6: for (int i = 0; i < 16; i++) tmp[i] = (uint32_t)src[i * 2] << 24 | (uint32_t)src[i * 2 + 1] << 16 | (uint32_t)src[32 + i * 2] << 8 | src[32 + i * 2 + 1]; break;
        case 14:                                              /* CMPR: four 4x4 DXT1 sub-blocks */
            for (int sb = 0; sb < 4; sb++) {
                const uint8_t *blk = src + sb * 8;
                uint16_t c0 = (uint16_t)(blk[0] << 8 | blk[1]), c1 = (uint16_t)(blk[2] << 8 | blk[3]);
                uint32_t p0 = c565_lut[c0], p1 = c565_lut[c1], pal[4];
                pal[0] = p0; pal[1] = p1;
                uint32_t r0 = p0 >> 16 & 255, g0 = p0 >> 8 & 255, b0 = p0 & 255, r1 = p1 >> 16 & 255, g1 = p1 >> 8 & 255, b1 = p1 & 255;
                if (c0 > c1) { pal[2] = rgba((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255); pal[3] = rgba((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255); }
                else { pal[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255); pal[3] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0); }
                int ox = (sb & 1) * 4, oy = (sb >> 1) * 4;
                for (int y = 0; y < 4; y++) { uint8_t row = blk[4 + y]; for (int x = 0; x < 4; x++) tmp[(oy + y) * 8 + ox + x] = pal[(row >> (6 - 2 * x)) & 3]; }
            }
            break;
        }
        uint32_t px0 = bx * tw, py0 = by * th;
        if (px0 + tw <= w && py0 + th <= h) {
            for (int y = 0; y < th; y++) memcpy(out + (size_t)(py0 + y) * w + px0, tmp + y * tw, tw * 4);
        } else {
            for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) if (px0 + x < w && py0 + y < h) out[(size_t)(py0 + y) * w + px0 + x] = tmp[y * tw + x];
        }
    }
    return 1;
}
/* decode block rows [by0, by1) of t into t->px */
static void decode_rows(texent *t, uint32_t by0, uint32_t by1) {
    if (decode_rows_fast(t, by0, by1)) return;
    uint32_t w = t->w, h = t->h, fmt = t->fmt;
    static const int bw[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
    static const int bh[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
    int tw = bw[fmt & 15] ? bw[fmt & 15] : 4, th = bh[fmt & 15] ? bh[fmt & 15] : 4;
    uint32_t xb = (w + tw - 1) / tw;
    const uint8_t *src = g_base + (t->addr & 0x3FFFFFFF) + (size_t)by0 * xb * ((fmt == 6) ? 64 : 32);
    uint32_t *out = t->px;
    for (uint32_t by = by0; by < by1; by++) for (uint32_t bx = 0; bx < xb; bx++) {
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
            uint32_t px = bx * tw + x, py = by * th + y;
            uint32_t c = 0;
            switch (fmt) {
            case 0: { uint8_t b = src[(y * tw + x) / 2]; uint8_t i = (x & 1) ? b & 15 : b >> 4; i *= 17; c = rgba(i, i, i, i); break; }
            case 1: { uint8_t i = src[y * tw + x]; c = rgba(i, i, i, i); break; }
            case 2: { uint8_t b = src[y * tw + x]; uint8_t i = (b & 15) * 17, a = (b >> 4) * 17; c = rgba(i, i, i, a); break; }
            case 3: { const uint8_t *p = src + (y * tw + x) * 2; c = rgba(p[1], p[1], p[1], p[0]); break; }
            case 4: { const uint8_t *p = src + (y * tw + x) * 2; c = c565((uint16_t)(p[0] << 8 | p[1])); break; }
            case 5: { const uint8_t *p = src + (y * tw + x) * 2; c = c5a3((uint16_t)(p[0] << 8 | p[1])); break; }
            case 6: { int i = y * 4 + x; c = rgba(src[i * 2 + 1], src[32 + i * 2], src[32 + i * 2 + 1], src[i * 2]); break; }
            case 8: { uint8_t b = src[(y * tw + x) / 2]; uint32_t i = (x & 1) ? b & 15 : b >> 4; c = tlut_color(t->tlut, t->tlutfmt, i); break; }
            case 9: { c = tlut_color(t->tlut, t->tlutfmt, src[y * tw + x]); break; }
            case 10: { const uint8_t *p = src + (y * tw + x) * 2; c = tlut_color(t->tlut, t->tlutfmt, ((p[0] << 8) | p[1]) & 0x3FFF); break; }
            case 14: {                                   /* CMPR: 2x2 DXT1 sub-blocks */
                const uint8_t *blk = src + ((y / 4) * 2 + (x / 4)) * 8;
                uint16_t c0 = (uint16_t)(blk[0] << 8 | blk[1]), c1 = (uint16_t)(blk[2] << 8 | blk[3]);
                uint32_t p0 = c565(c0), p1 = c565(c1), pal[4];
                pal[0] = p0; pal[1] = p1;
                uint8_t r0 = p0 >> 16, g0 = p0 >> 8, b0 = p0, r1 = p1 >> 16, g1 = p1 >> 8, b1 = p1;
                if (c0 > c1) {
                    pal[2] = rgba((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
                    pal[3] = rgba((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
                } else {
                    pal[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
                    pal[3] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0);
                }
                uint8_t row = blk[4 + (y & 3)];
                c = pal[(row >> (6 - 2 * (x & 3))) & 3];
                break;
            }
            default: c = 0xFFFF00FF; break;
            }
            if (px < w && py < h) out[py * w + px] = c;
        }
        src += (fmt == 6) ? 64 : 32;
    }
}
uint64_t g_decode_ns, g_compile_ns;                          /* slow-frame breakdown */
static void decode_texture(texent *t) {
    uint64_t t0 = rt_now_ns();
    static const int bh[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
    int th = bh[t->fmt & 15] ? bh[t->fmt & 15] : 4;
    defer_free(t->px);                          /* queued triangles may still sample the old one */
    t->ver = ++tex_ver;
    t->px = malloc((size_t)t->w * t->h * 4);
    decode_rows(t, 0, (t->h + th - 1) / th);
    g_decode_ns += rt_now_ns() - t0;
}

/* Texture cache. GXInvalidateTexAll (BP 0x0F) doesn't throw textures away: it starts a new
 * "epoch", and each cached texture is re-validated once per epoch by hashing its source memory
 * (and palette); only textures whose bytes actually changed are decoded again (as Dolphin does). */
static uint64_t tex_epoch = 1;
static uint64_t hash_bytes(const uint8_t *p, size_t n) { return XXH3_64bits(p, n); }
/* validation hash: large textures are sampled at 128 evenly spaced 32-byte blocks (Dolphin's
   default "Fast" texture cache accuracy uses 128 samples too) */
static uint64_t hash_sampled(const uint8_t *p, size_t n) {
    if (n <= 128 * 32) return XXH3_64bits(p, n);
    uint8_t buf[128 * 32]; size_t blocks = n / 32;
    for (int i = 0; i < 128; i++) memcpy(buf + i * 32, p + (blocks - 1) * i / 127 * 32, 32);
    return XXH3_64bits(buf, sizeof buf) ^ n;
}
/* CPU writes the GPU will see are flushed from the data cache first (DCFlushRange/DCStoreRange):
   remember the flushed ranges, so a texture whose memory was flushed since its last check gets
   a full hash (small in-place updates like animated tiles slip between the 128 samples) */
static size_t tex_bytes(uint32_t w, uint32_t h, uint32_t fmt) {
    static const int bw[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
    static const int bh[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
    int tw = bw[fmt & 15] ? bw[fmt & 15] : 4, th = bh[fmt & 15] ? bh[fmt & 15] : 4;
    return (size_t)((w + tw - 1) / tw) * ((h + th - 1) / th) * (fmt == 6 ? 64 : 32);
}
#define DIRTY_RING 4096
static struct { uint32_t a, b; } dirty[DIRTY_RING]; static uint64_t dirty_gen;
void gx_mark_dirty(uint32_t addr, uint32_t len) {
    if (!len) return;
    uint32_t a = addr & 0x3FFFFFFF;
    dirty[dirty_gen % DIRTY_RING].a = a; dirty[dirty_gen % DIRTY_RING].b = a + len; dirty_gen++;
}
static int dirty_since(uint64_t gen, uint32_t a, uint32_t b) {
    if (dirty_gen - gen >= DIRTY_RING) return 1;            /* too many to tell: assume so */
    for (uint64_t g = gen; g < dirty_gen; g++) { uint32_t i = (uint32_t)(g % DIRTY_RING); if (dirty[i].a < b && dirty[i].b > a) return 1; }
    return 0;
}
static int hash_full_next;                                  /* the next texture_hash covers every byte */
static uint64_t texture_hash(uint32_t addr, uint32_t w, uint32_t h, uint32_t fmt, uint32_t tlut) {
    static const int bw[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
    static const int bh[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
    int tw = bw[fmt & 15] ? bw[fmt & 15] : 4, th = bh[fmt & 15] ? bh[fmt & 15] : 4;
    size_t bytes = (size_t)((w + tw - 1) / tw) * ((h + th - 1) / th) * (fmt == 6 ? 64 : 32);
    { uint32_t pa = addr & 0x3FFFFFFF; if (!((pa + bytes <= 0x01900000u) || (pa >= 0x10000000u && pa + bytes <= 0x14000000u))) bytes = 0; }
    static int full = -1; if (full < 0) full = getenv("RT_TEXFULLHASH") != NULL;
    uint64_t hv = (full || hash_full_next) ? XXH3_64bits(g_base + (addr & 0x3FFFFFFF), bytes) ^ bytes : hash_sampled(g_base + (addr & 0x3FFFFFFF), bytes);
    hash_full_next = 0;
    if (fmt >= 8 && fmt <= 10) {                       /* palette lives in TMEM */
        size_t n = fmt == 8 ? 16 * 2 : fmt == 9 ? 256 * 2 : 16384 * 2, o = tlut & (sizeof tmem - 1);
        if (o + n > sizeof tmem) n = sizeof tmem - o;
        hv ^= hash_bytes(tmem + o, n) * 0x9E3779B97F4A7C15ull;
    }
    return hv;
}
/* Large textures the CPU edits in place (tilesets: animated tiles are copied into them every
   few frames, without a cache flush) are hashed in bands of block rows; only bands that changed
   are decoded again and re-uploaded (glTexSubImage2D of those rows). */
#define BAND_ROWS 4
static int band_eligible(texent *t) {
    size_t n = tex_bytes(t->w, t->h, t->fmt);
    uint32_t pa = t->addr & 0x3FFFFFFF;
    if (!((pa + n <= 0x01900000u) || (pa >= 0x10000000u && pa + n <= 0x14000000u))) return 0;
    return n >= 128 * 1024 && !(t->fmt >= 8 && t->fmt <= 10) && !t->hd;
}
static void band_hashes(texent *t, int compare) {
    static const int bh[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
    int th = bh[t->fmt & 15] ? bh[t->fmt & 15] : 4;
    uint32_t yb = (t->h + th - 1) / th, rowbytes = (uint32_t)(tex_bytes(t->w, t->h, t->fmt) / yb);
    uint32_t nb = (yb + BAND_ROWS - 1) / BAND_ROWS;
    if (!t->bandh || t->nbands != nb) { free(t->bandh); t->bandh = calloc(nb, 8); t->nbands = nb; compare = 0; }
    const uint8_t *src = g_base + (t->addr & 0x3FFFFFFF);
    uint32_t y0 = ~0u, y1 = 0;
    for (uint32_t b = 0; b < nb; b++) {
        uint32_t r0 = b * BAND_ROWS, r1 = r0 + BAND_ROWS < yb ? r0 + BAND_ROWS : yb;
        uint64_t hv = XXH3_64bits(src + (size_t)r0 * rowbytes, (size_t)(r1 - r0) * rowbytes);
        if (compare && hv != t->bandh[b]) { decode_rows(t, r0, r1); if (r0 * th < y0) y0 = r0 * th; if (r1 * th > y1) y1 = r1 * th; }
        t->bandh[b] = hv;
    }
    if (y1 > t->h) y1 = t->h;
    { static int full; if (!compare && !full && t->w == 1024 && t->h == 1024 && getenv("RT_TEXDIRTYLOG") && getenv("RT_DUMP_DIR")) { full = 1; char pth[1200]; snprintf(pth, sizeof pth, "%s/texfull.ppm", getenv("RT_DUMP_DIR"));
          FILE *f = fopen(pth, "wb"); if (f) { fprintf(f, "P6\n%u %u\n255\n", t->w, t->h); for (uint32_t i = 0; i < t->w * t->h; i++) { uint32_t c = t->px[i]; fputc(c >> 16, f); fputc(c >> 8, f); fputc(c, f); } fclose(f); } } }
    { static int lg = -1; if (lg < 0) lg = getenv("RT_TEXDIRTYLOG") != NULL;
      if (lg && y0 < y1) rt_log("texd", "%08X %ux%u f%u rows %u-%u changed", t->addr, t->w, t->h, t->fmt, y0, y1);
      static int nd; if (lg && y0 < y1 && nd < 8 && getenv("RT_DUMP_DIR")) { char pth[1200]; snprintf(pth, sizeof pth, "%s/texrows_%d.ppm", getenv("RT_DUMP_DIR"), nd++);
          FILE *f = fopen(pth, "wb"); if (f) { fprintf(f, "P6\n%u %u\n255\n", t->w, y1 - y0); for (uint32_t i = y0 * t->w; i < y1 * t->w; i++) { uint32_t c = t->px[i]; fputc(c >> 16, f); fputc(c >> 8, f); fputc(c, f); } fclose(f); } } }
    if (y0 < y1) {                                  /* rows to re-upload */
        if (t->pver && t->upd_y1 > t->upd_y0) { if (t->upd_y0 < y0) y0 = t->upd_y0; if (t->upd_y1 > y1) y1 = t->upd_y1; }
        t->upd_y0 = y0; t->upd_y1 = y1; t->pver++;
    }
}
/* decode a GX texture in guest memory to 0xAARRGGBB pixels (malloc'd; for the runtime's own UI) */
uint32_t *gx_decode_rgba(uint32_t addr, int w, int h, int fmt) {
    texent t; memset(&t, 0, sizeof t);
    t.addr = addr & 0x3FFFFFFF; t.w = (uint32_t)w; t.h = (uint32_t)h; t.fmt = (uint32_t)fmt;
    decode_texture(&t);
    return t.px;
}

/* custom texture (Dolphin pack) matching this texture's current contents, if any */
static void tex_hd_lookup(texent *t, int map) {
    t->hd = NULL;
    if (!hd_enabled()) return;
    static const int bw[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
    static const int bh[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
    int tw = bw[t->fmt & 15] ? bw[t->fmt & 15] : 4, th = bh[t->fmt & 15] ? bh[t->fmt & 15] : 4;
    size_t bytes = (size_t)((t->w + tw - 1) / tw) * ((t->h + th - 1) / th) * (t->fmt == 6 ? 64 : 32);
    { uint32_t pa = t->addr & 0x3FFFFFFF; if (!((pa + bytes <= 0x01900000u) || (pa >= 0x10000000u && pa + bytes <= 0x14000000u))) return; }
    int g = map >> 2, i = map & 3; uint32_t mode0 = bp((g ? 0xA0 : 0x80) + i);
    size_t to = t->tlut & (sizeof tmem - 1);
    t->hd = hd_lookup(g_base + (t->addr & 0x3FFFFFFF), bytes, tmem + to, sizeof tmem - to, t->w, t->h, t->fmt, ((mode0 >> 5) & 3) != 0);
}
static texent *get_texture(int map) {
    int g = map >> 2, i = map & 3;
    uint32_t img0 = bp((g ? 0xA8 : 0x88) + i), img3 = bp((g ? 0xB4 : 0x94) + i), tl = bp((g ? 0xB8 : 0x98) + i);
    uint32_t w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
    uint32_t addr = (img3 & 0xFFFFFF) << 5;
    uint32_t tlut = (tl & 0x3FF) << 9, tlutfmt = (tl >> 10) & 3;
    uint32_t key = (addr >> 5) ^ (w * 31) ^ (h * 977) ^ (fmt * 7919);
    texent *best = NULL;
    for (int k = 0; k < 8; k++) {
        texent *t = &texcache[(key + k) % TEXCACHE];
        if (t->px && t->addr == addr && t->w == w && t->h == h && t->fmt == fmt && (fmt < 8 || fmt > 10 || (t->tlut == tlut && t->tlutfmt == tlutfmt))) {
            t->stamp = tex_stamp;
            if (t->epoch != tex_epoch && t->bandh) { band_hashes(t, 1); t->epoch = tex_epoch; return t; }
            if (t->epoch != tex_epoch) {               /* first use since an invalidate: did the bytes change? */
                if (dirty_since(t->dgen, t->addr & 0x3FFFFFFF, (t->addr & 0x3FFFFFFF) + tex_bytes(w, h, fmt))) hash_full_next = 1;
                int wasfull = hash_full_next;
                uint64_t dg0 = t->dgen; t->dgen = dirty_gen;
                uint64_t hv = texture_hash(addr, w, h, fmt, tlut);
                { static int lg = -1; if (lg < 0) lg = getenv("RT_TEXDIRTYLOG") != NULL;
                  if (lg && w * h >= 256 * 256) rt_log("texd", "%08X %ux%u f%u: flushes since %llu, full %d, changed %d", addr, w, h, fmt, (unsigned long long)(dirty_gen - dg0), wasfull, hv != t->hash); }
                if (hv != t->hash) { t->hash = hv; decode_texture(t); tex_hd_lookup(t, map); }
                t->epoch = tex_epoch;
            }
            return t;
        }
        if (!best || t->stamp < best->stamp) best = t;
    }
    best->addr = addr; best->w = w; best->h = h; best->fmt = fmt; best->tlut = tlut; best->tlutfmt = tlutfmt; best->stamp = tex_stamp;
    { extern int g_copystats; extern uint32_t g_copydst[64];
      if (g_copystats) for (int i = 0; i < 64; i++) if (g_copydst[i] == addr) {
          static uint32_t seen[256]; static int nseen; uint32_t key = addr ^ fmt << 28 ^ w << 4 ^ h << 16; int f = 0;
          for (int j = 0; j < nseen; j++) if (seen[j] == key) f = 1;
          if (!f && nseen < 256) { seen[nseen++] = key; rt_log("copy", "  texture read of copy %08X as fmt %u %ux%u", addr, fmt, w, h); }
          break; } }
    free(best->bandh); best->bandh = NULL; best->nbands = 0; best->pver = 0; best->upd_y0 = best->upd_y1 = 0;
    hash_full_next = 1; best->dgen = dirty_gen;
    best->hash = texture_hash(addr, w, h, fmt, tlut); best->epoch = tex_epoch;
    decode_texture(best);
    tex_hd_lookup(best, map);
    if (band_eligible(best)) band_hashes(best, 0);
    return best;
}
void gx_render_tex_invalidate(void) { tex_epoch++; }
void gx_render_tlut_load(int reg, uint32_t val) {
    if (reg != 0x65) return;
    uint32_t src = (bp(0x64) & 0xFFFFFF) << 5, dst = (val & 0x3FF) << 9, cnt = ((val >> 10) & 0x7FF) * 32;
    if (dst + cnt > sizeof tmem) cnt = sizeof tmem - dst;
    memcpy(tmem + dst, g_base + (src & 0x3FFFFFFF), cnt);
    tex_epoch++;                               /* palette textures re-validate (their hash covers the palette) */
}

/* ================================================================== vertex processing */
typedef struct {
    float x, y, z, w;            /* clip space */
    float sx, sy, sz;            /* screen */
    float col[2][4];             /* 0..255 */
    float tex[8][3];             /* s, t, q */
} vtx;

typedef struct { float pos[3], nrm[3], bin[3], tan[3]; float col[2][4]; float tc[8][2]; int pnidx; int tmidx[8]; } rawv;

static float read_comp(const uint8_t **p, int fmt, int shift) {
    const uint8_t *q = *p; float v;
    switch (fmt) {
    case 0: v = q[0]; *p += 1; break;
    case 1: v = (int8_t)q[0]; *p += 1; break;
    case 2: v = (uint16_t)(q[0] << 8 | q[1]); *p += 2; break;
    case 3: v = (int16_t)(q[0] << 8 | q[1]); *p += 2; break;
    default: { uint32_t u = (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3]; float f; memcpy(&f, &u, 4); *p += 4; return f; }
    }
    return v / (float)(1 << shift);
}
static void read_color(const uint8_t **p, int fmt, float out[4]) {
    const uint8_t *q = *p; uint32_t c;
    switch (fmt) {
    case 0: c = c565((uint16_t)(q[0] << 8 | q[1])); *p += 2; out[0] = (c >> 16) & 255; out[1] = (c >> 8) & 255; out[2] = c & 255; out[3] = 255; return;
    case 1: out[0] = q[0]; out[1] = q[1]; out[2] = q[2]; out[3] = 255; *p += 3; return;
    case 2: out[0] = q[0]; out[1] = q[1]; out[2] = q[2]; out[3] = 255; *p += 4; return;
    case 3: out[0] = (q[0] >> 4) * 17; out[1] = (q[0] & 15) * 17; out[2] = (q[1] >> 4) * 17; out[3] = (q[1] & 15) * 17; *p += 2; return;
    case 4: { uint32_t v = q[0] << 16 | q[1] << 8 | q[2]; out[0] = ((v >> 18) & 63) * 255 / 63; out[1] = ((v >> 12) & 63) * 255 / 63;
              out[2] = ((v >> 6) & 63) * 255 / 63; out[3] = (v & 63) * 255 / 63; *p += 3; return; }
    default: out[0] = q[0]; out[1] = q[1]; out[2] = q[2]; out[3] = q[3]; *p += 4; return;
    }
}
static const uint8_t *indexed(int arr, const uint8_t **p, int type) {
    uint32_t idx;
    if (type == 2) { idx = (*p)[0]; *p += 1; } else { idx = (uint32_t)((*p)[0] << 8 | (*p)[1]); *p += 2; }
    return g_base + ((gx.cp_array_base[arr] + idx * gx.cp_array_stride[arr]) & 0x3FFFFFFF);
}

static void decode_vertex(int vat, const uint8_t **pp, rawv *v) {
    const uint8_t *p = *pp;
    uint32_t lo = gx.cp_vcd_lo, hi = gx.cp_vcd_hi, a = gx.cp_vat_a[vat], b = gx.cp_vat_b[vat], c = gx.cp_vat_c[vat];
    memset(v, 0, sizeof *v);
    /* default matrix indices come from the XF copy of MATINDEX (0x1018/0x1019), like Dolphin: G3D updates
       only the XF registers (via display lists), so the CP copy can be stale */
    uint32_t mia = gx.xf_regs[0x18], mib = gx.xf_regs[0x19];
    v->pnidx = mia & 63;
    for (int i = 0; i < 8; i++) v->tmidx[i] = i < 4 ? (mia >> (6 + 6 * i)) & 63 : (mib >> (6 * (i - 4))) & 63;
    if (lo & 1) v->pnidx = *p++ & 63;
    for (int i = 0; i < 8; i++) if (lo & (2u << i)) v->tmidx[i] = *p++ & 63;
    /* position */
    int pt = (lo >> 9) & 3;
    if (pt) {
        const uint8_t *q = pt == 1 ? p : indexed(0, &p, pt);
        int cnt = (a & 1) ? 3 : 2, fmt = (a >> 1) & 7, sh = (a >> 4) & 31;
        for (int i = 0; i < cnt; i++) v->pos[i] = read_comp(&q, fmt, sh);
        if (pt == 1) p = q;
    }
    /* normal (+binormal/tangent) */
    int nt = (lo >> 11) & 3;
    if (nt) {
        int nbt = (a >> 9) & 1, fmt = (a >> 10) & 7, idx3 = (a >> 31) & 1;
        int sh = (fmt == 1) ? 6 : (fmt == 3) ? 14 : 0;
        float *dst[3] = {v->nrm, v->bin, v->tan};
        if (nt == 1) { for (int k = 0; k < (nbt ? 3 : 1); k++) for (int i = 0; i < 3; i++) dst[k][i] = read_comp(&p, fmt, sh); }
        else if (nbt && idx3) { for (int k = 0; k < 3; k++) { const uint8_t *q = indexed(1, &p, nt); q += k * 3 * (fmt >= 4 ? 4 : fmt >= 2 ? 2 : 1); for (int i = 0; i < 3; i++) dst[k][i] = read_comp(&q, fmt, sh); } }
        else { const uint8_t *q = indexed(1, &p, nt); for (int k = 0; k < (nbt ? 3 : 1); k++) for (int i = 0; i < 3; i++) dst[k][i] = read_comp(&q, fmt, sh); }
    }
    /* colors */
    for (int ci = 0; ci < 2; ci++) {
        int ct = (lo >> (13 + 2 * ci)) & 3, fmt = (a >> (14 + 4 * ci)) & 7;
        if (!ct) { v->col[ci][0] = v->col[ci][1] = v->col[ci][2] = v->col[ci][3] = 255; continue; }
        if (ct == 1) read_color(&p, fmt, v->col[ci]);
        else { const uint8_t *q = indexed(2 + ci, &p, ct); read_color(&q, fmt, v->col[ci]); }
    }
    /* texture coordinates */
    for (int t = 0; t < 8; t++) {
        int tt = (hi >> (2 * t)) & 3; if (!tt) continue;
        int cnt, fmt, sh;
        switch (t) {
        case 0: cnt = (a >> 21) & 1; fmt = (a >> 22) & 7; sh = (a >> 25) & 31; break;
        case 1: cnt = b & 1; fmt = (b >> 1) & 7; sh = (b >> 4) & 31; break;
        case 2: cnt = (b >> 9) & 1; fmt = (b >> 10) & 7; sh = (b >> 13) & 31; break;
        case 3: cnt = (b >> 18) & 1; fmt = (b >> 19) & 7; sh = (b >> 22) & 31; break;
        case 4: cnt = (b >> 27) & 1; fmt = (b >> 28) & 7; sh = c & 31; break;
        case 5: cnt = (c >> 5) & 1; fmt = (c >> 6) & 7; sh = (c >> 9) & 31; break;
        case 6: cnt = (c >> 14) & 1; fmt = (c >> 15) & 7; sh = (c >> 18) & 31; break;
        default: cnt = (c >> 23) & 1; fmt = (c >> 24) & 7; sh = (c >> 27) & 31; break;
        }
        const uint8_t *q = tt == 1 ? p : indexed(4 + t, &p, tt);
        v->tc[t][0] = read_comp(&q, fmt, sh);
        if (cnt) v->tc[t][1] = read_comp(&q, fmt, sh);
        if (tt == 1) p = q;
    }
    *pp = p;
}

static void mtx_mul34(uint32_t addr, const float in[4], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = xf_f(addr + r * 4) * in[0] + xf_f(addr + r * 4 + 1) * in[1] + xf_f(addr + r * 4 + 2) * in[2] + xf_f(addr + r * 4 + 3) * in[3];
}

static void light_channel(int chan, const float pos[3], const float n[3], const float vcol[4], float out[4]) {
    for (int comp = 0; comp < 2; comp++) {                /* 0 = color (rgb), 1 = alpha */
        uint32_t ctl = xf_r(0x0E + chan + 2 * comp);
        uint32_t matr = xf_r(0x0C + chan), ambr = xf_r(0x0A + chan);
        float mat[4] = {(float)(matr >> 24), (float)((matr >> 16) & 255), (float)((matr >> 8) & 255), (float)(matr & 255)};
        float amb[4] = {(float)(ambr >> 24), (float)((ambr >> 16) & 255), (float)((ambr >> 8) & 255), (float)(ambr & 255)};
        if (ctl & 1) memcpy(mat, vcol, sizeof mat);
        if (ctl & 0x40) memcpy(amb, vcol, sizeof amb);
        float res[4];
        if (!(ctl & 2)) memcpy(res, mat, sizeof res);
        else {
            float acc[4] = {amb[0], amb[1], amb[2], amb[3]};
            uint32_t mask = ((ctl >> 2) & 15) | (((ctl >> 11) & 15) << 4);
            int dfn = (ctl >> 7) & 3, attn_en = (ctl >> 9) & 1, spot = (ctl >> 10) & 1;
            for (int l = 0; l < 8; l++) if (mask & (1u << l)) {
                uint32_t base = 0x600 + l * 0x10;
                uint32_t lc = gx.xf_mem[base + 3];
                float lcol[4] = {(float)(lc >> 24), (float)((lc >> 16) & 255), (float)((lc >> 8) & 255), (float)(lc & 255)};
                float lp[3] = {xf_f(base + 10), xf_f(base + 11), xf_f(base + 12)};
                float ld[3] = {xf_f(base + 13), xf_f(base + 14), xf_f(base + 15)};
                float L[3] = {lp[0] - pos[0], lp[1] - pos[1], lp[2] - pos[2]};
                float dist2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2], dist = sqrtf(dist2);
                if (dist > 0) { L[0] /= dist; L[1] /= dist; L[2] /= dist; }
                float ndl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
                float d = dfn == 0 ? 1.0f : dfn == 1 ? ndl : (ndl > 0 ? ndl : 0);
                float att = 1.0f;
                if (attn_en) {
                    float a0 = xf_f(base + 4), a1 = xf_f(base + 5), a2 = xf_f(base + 6), k0 = xf_f(base + 7), k1 = xf_f(base + 8), k2 = xf_f(base + 9);
                    if (spot) {
                        float cs = L[0] * ld[0] + L[1] * ld[1] + L[2] * ld[2];   /* L points at the light */
                        if (cs < 0) cs = 0;
                        float ca = a0 + a1 * cs + a2 * cs * cs; if (ca < 0) ca = 0;
                        float da = k0 + k1 * dist + k2 * dist2;
                        att = da != 0 ? ca / da : 0;
                    } else {                                     /* specular */
                        float hn = n[0] * ld[0] + n[1] * ld[1] + n[2] * ld[2];
                        if (ndl <= 0) hn = 0; if (hn < 0) hn = 0;
                        float ca = a0 + a1 * hn + a2 * hn * hn, da = k0 + k1 * hn + k2 * hn * hn;
                        att = da != 0 ? ca / da : 0; if (att < 0) att = 0;
                    }
                }
                for (int k = 0; k < 4; k++) acc[k] += lcol[k] * d * att;
            }
            for (int k = 0; k < 4; k++) { float c = acc[k] < 0 ? 0 : acc[k] > 255 ? 255 : acc[k]; res[k] = mat[k] * c / 255.0f; }
        }
        if (comp == 0) { out[0] = res[0]; out[1] = res[1]; out[2] = res[2]; } else out[3] = res[3];
    }
}

static void transform_vertex(const rawv *r, vtx *o) {
    float in[4] = {r->pos[0], r->pos[1], r->pos[2], 1.0f}, mv[3];
    mtx_mul34((uint32_t)r->pnidx * 4, in, mv);
    float nv[3] = {0, 0, 1};
    int nchan = xf_r(0x09) & 3;
    if (nchan) {                                              /* (the eye-space normal is only used for lighting) */
        uint32_t na = 0x400 + (uint32_t)r->pnidx * 3;
        for (int i = 0; i < 3; i++) nv[i] = xf_f(na + i * 3) * r->nrm[0] + xf_f(na + i * 3 + 1) * r->nrm[1] + xf_f(na + i * 3 + 2) * r->nrm[2];
        float nl = sqrtf(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]); if (nl > 0) { nv[0] /= nl; nv[1] /= nl; nv[2] /= nl; }
    }
    /* projection */
    float p0 = xf_f(0x1020), p1 = xf_f(0x1021), p2 = xf_f(0x1022), p3 = xf_f(0x1023), p4 = xf_f(0x1024), p5 = xf_f(0x1025);
    if (xf_r(0x26)) { o->x = p0 * mv[0] + p1; o->y = p2 * mv[1] + p3; o->z = p4 * mv[2] + p5; o->w = 1.0f; }
    else { o->x = p0 * mv[0] + p1 * mv[2]; o->y = p2 * mv[1] + p3 * mv[2]; o->z = p4 * mv[2] + p5; o->w = -mv[2]; }
    /* colors */
    for (int c = 0; c < 2; c++) {
        if (c < nchan) light_channel(c, mv, nv, r->col[c], o->col[c]);
        else memcpy(o->col[c], r->col[c], sizeof o->col[c]);
    }
    /* texgen */
    int ntex = xf_r(0x3F) & 15;
    for (int t = 0; t < ntex && t < 8; t++) {
        uint32_t info = xf_r(0x40 + t), post = xf_r(0x50 + t);
        int proj = (info >> 1) & 1, form = (info >> 2) & 1, type = (info >> 4) & 7, row = (info >> 7) & 31;
        float src[4] = {0, 0, 1, 1};
        if (row == 0) { src[0] = r->pos[0]; src[1] = r->pos[1]; src[2] = r->pos[2]; }
        else if (row == 1) { src[0] = r->nrm[0]; src[1] = r->nrm[1]; src[2] = r->nrm[2]; }
        else if (row == 3) { memcpy(src, r->tan, 12); }
        else if (row == 4) { memcpy(src, r->bin, 12); }
        else if (row >= 5 && row <= 12) { src[0] = r->tc[row - 5][0]; src[1] = r->tc[row - 5][1]; src[2] = 1; }
        if (!form) src[2] = 1.0f;
        float out[3] = {0, 0, 1};
        if (type == 0) {
            uint32_t ma = (uint32_t)r->tmidx[t] * 4;
            out[0] = xf_f(ma) * src[0] + xf_f(ma + 1) * src[1] + xf_f(ma + 2) * src[2] + xf_f(ma + 3) * src[3];
            out[1] = xf_f(ma + 4) * src[0] + xf_f(ma + 5) * src[1] + xf_f(ma + 6) * src[2] + xf_f(ma + 7) * src[3];
            out[2] = proj ? xf_f(ma + 8) * src[0] + xf_f(ma + 9) * src[1] + xf_f(ma + 10) * src[2] + xf_f(ma + 11) * src[3] : 1.0f;
        } else if (type == 2 || type == 3) {
            out[0] = o->col[type - 2][0] / 255.0f; out[1] = o->col[type - 2][1] / 255.0f; out[2] = 1;
        } else { out[0] = src[0]; out[1] = src[1]; out[2] = 1; }
        if (xf_r(0x12) & 1) {                                    /* dual-tex post transform */
            if (post & 0x100) { float l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]); if (l > 0) { out[0] /= l; out[1] /= l; out[2] /= l; } }
            uint32_t pa = 0x500 + (post & 63) * 4;
            float in2[4] = {out[0], out[1], out[2], 1}, res[3];
            mtx_mul34(pa, in2, res);
            out[0] = res[0]; out[1] = res[1]; out[2] = res[2];
        }
        o->tex[t][0] = out[0]; o->tex[t][1] = out[1]; o->tex[t][2] = out[2];
    }
}

static void to_screen(vtx *v) {
    float iw = v->w != 0 ? 1.0f / v->w : 0;
    float sx = xf_f(0x101A), sy = xf_f(0x101B), sz = xf_f(0x101C), ox = xf_f(0x101D), oy = xf_f(0x101E), oz = xf_f(0x101F);
    v->sx = v->x * iw * sx + ox - 342.0f;
    v->sy = v->y * iw * sy + oy - 342.0f;
    v->sz = v->z * iw * sz + oz;
    float so_x = (float)((bp(0x59) & 0x3FF) * 2) - 342.0f, so_y = (float)(((bp(0x59) >> 10) & 0x3FF) * 2) - 342.0f;
    v->sx -= so_x; v->sy -= so_y;
}

/* ================================================================== TEV + pixel pipeline
 * Each draw's state is decoded once into a drawstate; triangles are set up with plane
 * equations and queued. The queue is rasterized in parallel horizontal bands (each band
 * walks every triangle in submission order, so per-pixel ordering is preserved) whenever
 * the EFB is about to be read: EFB copies and draw-done. */
typedef struct { int r, g, b, a; } ic;
static ic tev_reg[4], tev_konst[4];
static uint32_t frame_draws;
/* RT_GXSTATS: per-frame counts of where triangles/pixels are dropped (approximate across threads) */
static int gxstats = -1;
static uint64_t st_tri_in, st_tri_clip, st_tri_zero, st_tri_cull, st_tri_sc, st_tri_ok, st_px, st_px_z, st_px_a, st_px_w, st_persp;
uint64_t g_frames_presented;
/* performance breakdown, logged every 300 presented frames */
static double g_perf_drawcount;
uint64_t g_perf_draw_ns, g_perf_copy_ns, g_perf_present_ns, g_perf_ax_ns, g_perf_xform_ns, g_perf_key_ns;
static void perf_report(void) {
    static uint64_t t0, f0;
    uint64_t now = rt_now_ns();
    if (t0 && g_frames_presented > f0) {
        double n = (double)(g_frames_presented - f0), wall = (now - t0) / 1e6 / n;
        double d = g_perf_draw_ns / 1e6 / n, c = g_perf_copy_ns / 1e6 / n, pr = g_perf_present_ns / 1e6 / n, ax = g_perf_ax_ns / 1e6 / n;
        extern uint64_t g_perf_flush_ns; extern uint32_t g_perf_batches;
        rt_log("perf", "%.1f fps | per frame: %.2f ms total = draws %.2f (vertex decode/transform %.2f, shader lookup %.2f, GL submit %.2f in %.0f batches for %.0f draws) + efb copies %.2f + present %.2f + audio %.2f + game code/idle %.2f",
               1000.0 / wall, wall, d, g_perf_xform_ns / 1e6 / n, g_perf_key_ns / 1e6 / n, g_perf_flush_ns / 1e6 / n, g_perf_batches / n, g_perf_drawcount / n, c, pr, ax, wall - d - c - pr - ax);
        g_perf_flush_ns = 0; g_perf_batches = 0;
    }
    t0 = now; f0 = g_frames_presented; g_perf_drawcount = 0; g_perf_draw_ns = g_perf_copy_ns = g_perf_present_ns = g_perf_ax_ns = g_perf_xform_ns = g_perf_key_ns = 0;
}

static inline int tev_clamp(int v, int clamp) { return clamp ? (v < 0 ? 0 : v > 255 ? 255 : v) : (v < -1024 ? -1024 : v > 1023 ? 1023 : v); }
static inline int cmpf(int f, int a, int ref) {
    switch (f) { case 0: return 0; case 1: return a < ref; case 2: return a == ref; case 3: return a <= ref;
    case 4: return a > ref; case 5: return a != ref; case 6: return a >= ref; default: return 1; }
}
void gx_render_tev_reg(int reg, uint32_t v) {
    int i = (reg - 0xE0) >> 1, konst = (v >> 23) & 1;
    int lo = v & 0x7FF, hi = (v >> 12) & 0x7FF;
    if (lo & 0x400) lo -= 0x800; if (hi & 0x400) hi -= 0x800;
    ic *t = konst ? &tev_konst[i] : &tev_reg[i];
    if (!(reg & 1)) { t->r = konst ? (lo & 255) : lo; t->a = konst ? (hi & 255) : hi; }
    else { t->b = konst ? (lo & 255) : lo; t->g = konst ? (hi & 255) : hi; }
}
static ic konst_color(int sel) {
    ic c;
    if (sel < 8) { static const int v[8] = {255, 223, 191, 159, 128, 96, 64, 32}; c.r = c.g = c.b = c.a = v[sel]; return c; }
    if (sel >= 0x0C && sel <= 0x0F) return tev_konst[sel - 0x0C];
    if (sel < 0x10) { c.r = c.g = c.b = c.a = 0; return c; }
    int k = sel & 3, comp = (sel - 0x10) >> 2;
    int v = comp == 0 ? tev_konst[k].r : comp == 1 ? tev_konst[k].g : comp == 2 ? tev_konst[k].b : tev_konst[k].a;
    c.r = c.g = c.b = c.a = v; return c;
}
static int konst_alpha(int sel) {
    if (sel < 8) { static const int v[8] = {255, 223, 191, 159, 128, 96, 64, 32}; return v[sel]; }
    if (sel < 0x10) return 0;
    int k = sel & 3, comp = (sel - 0x10) >> 2;
    return comp == 0 ? tev_konst[k].r : comp == 1 ? tev_konst[k].g : comp == 2 ? tev_konst[k].b : tev_konst[k].a;
}

typedef struct {
    const uint32_t *px; int tw, th, ws, wt, lin, texen, coord, chan;
    uint8_t ca, cb, cc, cd, cbias, cop, cclamp, cscale, cdst;
    uint8_t aa, ab, ac, ad, abias, aop, aclamp, ascale, adst;
    uint8_t rsw[4], tsw[4];
    ic kc; int ka;
} stage_t;
typedef struct drawstate {
    int nstages, ntex, ncol;
    stage_t st[16];
    ic regs[4];
    int at_f0, at_f1, at_r0, at_r1, at_op, at_always;
    int zen, zfn, zupd;
    int blend, bsub, sf, df, cupd, aupd, dstconst_en, dstconst, has_alpha;
} drawstate;

typedef struct {
    const drawstate *ds;
    int minx, maxx, miny, maxy;
    float e[3][3];          /* edge functions: e[i][0]*x + e[i][1]*y + e[i][2] >= 0 inside */
    int nattr;              /* plane-equation attributes */
    float pa[40][3];        /* a*x + b*y + c */
    int persp;              /* attributes divided by interpolated 1/w */
    uint8_t tl[3];          /* top-left fill rule: pixels exactly on this edge belong to this triangle */
} tri_t;
/* attribute layout: 0 = z, 1 = 1/w, 2..9 = colors (premultiplied by 1/w if persp), 10.. = s,t,q per texcoord (/w) */

static drawstate *ds_pool; static int nds, capds;
static tri_t *tris; static int ntris, captris;

static void swap_table(int table, uint8_t out[4]) {
    uint32_t k0 = bp(0xF6 + table * 2), k1 = bp(0xF7 + table * 2);
    out[0] = k0 & 3; out[1] = (k0 >> 2) & 3; out[2] = k1 & 3; out[3] = (k1 >> 2) & 3;
}
static const drawstate *build_drawstate(void) {
    if (nds == capds) {
        /* drawstates are referenced by queued triangles: grow only between flushes */
        extern void gx_render_flush(void);
        if (nds) gx_render_flush();
        if (nds == capds) { capds = capds ? capds * 2 : 1024; ds_pool = realloc(ds_pool, sizeof(drawstate) * capds); }
    }
    drawstate *d = &ds_pool[nds++];
    uint32_t gen = bp(0x00);
    d->nstages = ((gen >> 10) & 15) + 1; d->ntex = gen & 15; d->ncol = 2;
    memcpy(d->regs, tev_reg, sizeof d->regs);
    for (int s = 0; s < d->nstages; s++) {
        stage_t *st = &d->st[s];
        uint32_t tref = bp(0x28 + s / 2) >> ((s & 1) * 12);
        int map = tref & 7;
        st->coord = (tref >> 3) & 7; st->texen = (tref >> 6) & 1; st->chan = (tref >> 7) & 7;
        st->px = NULL;
        if (st->texen) {
            texent *t = get_texture(map);
            int g = map >> 2, i = map & 3; uint32_t mode0 = bp((g ? 0xA0 : 0x80) + i);
            st->px = t->px; st->tw = t->w; st->th = t->h; st->ws = mode0 & 3; st->wt = (mode0 >> 2) & 3; st->lin = (mode0 >> 4) & 1;
        }
        uint32_t c = bp(0xC0 + 2 * s), a = bp(0xC1 + 2 * s);
        st->cd = c & 15; st->cc = (c >> 4) & 15; st->cb = (c >> 8) & 15; st->ca = (c >> 12) & 15;
        st->cbias = (c >> 16) & 3; st->cop = (c >> 18) & 1; st->cclamp = (c >> 19) & 1; st->cscale = (c >> 20) & 3; st->cdst = (c >> 22) & 3;
        st->ad = (a >> 4) & 7; st->ac = (a >> 7) & 7; st->ab = (a >> 10) & 7; st->aa = (a >> 13) & 7;
        st->abias = (a >> 16) & 3; st->aop = (a >> 18) & 1; st->aclamp = (a >> 19) & 1; st->ascale = (a >> 20) & 3; st->adst = (a >> 22) & 3;
        swap_table(a & 3, st->rsw); swap_table((a >> 2) & 3, st->tsw);
        uint32_t ksel = bp(0xF6 + s / 2) >> ((s & 1) ? 14 : 4);
        st->kc = konst_color(ksel & 31); st->ka = konst_alpha((ksel >> 5) & 31);
    }
    uint32_t ac = bp(0xF3);
    d->at_r0 = ac & 255; d->at_r1 = (ac >> 8) & 255; d->at_f0 = (ac >> 16) & 7; d->at_f1 = (ac >> 19) & 7; d->at_op = (ac >> 22) & 3;
    d->at_always = (d->at_op == 0 && d->at_f0 == 7 && d->at_f1 == 7) || (d->at_op == 1 && (d->at_f0 == 7 || d->at_f1 == 7));
    uint32_t zm = bp(0x40); d->zen = zm & 1; d->zfn = (zm >> 1) & 7; d->zupd = (zm >> 4) & 1;
    uint32_t cm = bp(0x41);
    d->blend = cm & 1; d->bsub = (cm >> 11) & 1; d->sf = (cm >> 8) & 7; d->df = (cm >> 5) & 7; d->cupd = (cm >> 3) & 1; d->aupd = (cm >> 4) & 1;
    d->dstconst_en = (bp(0x42) >> 8) & 1; d->dstconst = bp(0x42) & 255;
    d->has_alpha = (bp(0x43) & 7) == 1;
    return d;
}

static inline int wrapc(int c, int size, int mode) {
    if (mode == 1) { if ((size & (size - 1)) == 0) return c & (size - 1); c %= size; return c < 0 ? c + size : c; }
    if (mode == 2) { int p = size * 2; c %= p; if (c < 0) c += p; return c < size ? c : p - 1 - c; }
    return c < 0 ? 0 : c >= size ? size - 1 : c;
}
static inline uint32_t sample_st(const stage_t *st, float s, float t) {
    if (!st->lin) {
        int x = wrapc((int)floorf(s * st->tw), st->tw, st->ws), y = wrapc((int)floorf(t * st->th), st->th, st->wt);
        return st->px[y * st->tw + x];
    }
    float fx = s * st->tw - 0.5f, fy = t * st->th - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    int ax = (int)((fx - x0) * 256), ay = (int)((fy - y0) * 256);
    int xa = wrapc(x0, st->tw, st->ws), xb = wrapc(x0 + 1, st->tw, st->ws), ya = wrapc(y0, st->th, st->wt), yb = wrapc(y0 + 1, st->th, st->wt);
    const uint32_t *p = st->px; int w = st->tw;
    uint32_t c00 = p[ya * w + xa], c10 = p[ya * w + xb], c01 = p[yb * w + xa], c11 = p[yb * w + xb];
    if ((c00 == c10) & (c01 == c11) & (c00 == c01)) return c00;
    /* two channels at a time in 32-bit lanes */
    uint32_t rb00 = c00 & 0x00FF00FF, rb10 = c10 & 0x00FF00FF, rb01 = c01 & 0x00FF00FF, rb11 = c11 & 0x00FF00FF;
    uint32_t ag00 = (c00 >> 8) & 0x00FF00FF, ag10 = (c10 >> 8) & 0x00FF00FF, ag01 = (c01 >> 8) & 0x00FF00FF, ag11 = (c11 >> 8) & 0x00FF00FF;
    uint32_t rbt = (rb00 * (256 - ax) + rb10 * ax) >> 8 & 0x00FF00FF, rbb = (rb01 * (256 - ax) + rb11 * ax) >> 8 & 0x00FF00FF;
    uint32_t agt = (ag00 * (256 - ax) + ag10 * ax) >> 8 & 0x00FF00FF, agb = (ag01 * (256 - ax) + ag11 * ax) >> 8 & 0x00FF00FF;
    uint32_t rb = (rbt * (256 - ay) + rbb * ay) >> 8 & 0x00FF00FF, ag = (agt * (256 - ay) + agb * ay) >> 8 & 0x00FF00FF;
    return rb | (ag << 8);
}

static inline ic cin(int sel, const ic *reg, ic tex, ic ras, ic kc) {
    ic z = {0, 0, 0, 0};
    switch (sel) {
    case 0: return reg[0]; case 1: { ic c = {reg[0].a, reg[0].a, reg[0].a, 0}; return c; }
    case 2: return reg[1]; case 3: { ic c = {reg[1].a, reg[1].a, reg[1].a, 0}; return c; }
    case 4: return reg[2]; case 5: { ic c = {reg[2].a, reg[2].a, reg[2].a, 0}; return c; }
    case 6: return reg[3]; case 7: { ic c = {reg[3].a, reg[3].a, reg[3].a, 0}; return c; }
    case 8: return tex; case 9: { ic c = {tex.a, tex.a, tex.a, 0}; return c; }
    case 10: return ras; case 11: { ic c = {ras.a, ras.a, ras.a, 0}; return c; }
    case 12: { ic c = {255, 255, 255, 0}; return c; } case 13: { ic c = {128, 128, 128, 0}; return c; }
    case 14: return kc;
    }
    return z;
}
static inline int tev_comb(int a, int b, int c, int d, int bias, int op, int clamp, int scale) {
    a &= 255; b &= 255; c &= 255;
    int cc = c + (c >> 7);
    int lerp = (a * (256 - cc) + b * cc) >> 8;
    int v = op ? d - lerp : d + lerp;
    v += bias == 1 ? 128 : bias == 2 ? -128 : 0;
    v = scale == 1 ? v * 2 : scale == 2 ? v * 4 : scale == 3 ? v / 2 : v;
    return tev_clamp(v, clamp);
}

static void shade(const drawstate *d, uint32_t idx, const float *col, const float tcs[8][2], uint32_t z) {
    if (gxstats) st_px++;
    if (d->zen && !cmpf(d->zfn, (int)z, (int)zbuf[idx])) { if (gxstats) st_px_z++; return; }
    ic reg[4]; memcpy(reg, d->regs, sizeof reg);
    for (int s = 0; s < d->nstages; s++) {
        const stage_t *st = &d->st[s];
        ic tex = {255, 255, 255, 255}, ras = {0, 0, 0, 0};
        if (st->texen && st->px) {
            uint32_t t = sample_st(st, tcs[st->coord][0], tcs[st->coord][1]);
            int v[4] = {(int)((t >> 16) & 255), (int)((t >> 8) & 255), (int)(t & 255), (int)(t >> 24)};
            tex.r = v[st->tsw[0]]; tex.g = v[st->tsw[1]]; tex.b = v[st->tsw[2]]; tex.a = v[st->tsw[3]];
        }
        if (st->chan <= 1) {
            const float *c = col + st->chan * 4;
            int v[4] = {(int)c[0], (int)c[1], (int)c[2], (int)c[3]};
            ras.r = v[st->rsw[0]]; ras.g = v[st->rsw[1]]; ras.b = v[st->rsw[2]]; ras.a = v[st->rsw[3]];
        }
        ic A = cin(st->ca, reg, tex, ras, st->kc), B = cin(st->cb, reg, tex, ras, st->kc), C = cin(st->cc, reg, tex, ras, st->kc), D = cin(st->cd, reg, tex, ras, st->kc);
        int ai[8] = {reg[0].a, reg[1].a, reg[2].a, reg[3].a, tex.a, ras.a, st->ka, 0};
        int aA = ai[st->aa], aB = ai[st->ab], aC = ai[st->ac], aD = ai[st->ad];
        int orr, og, ob, oa;
        if (st->cbias != 3) {
            orr = tev_comb(A.r, B.r, C.r, D.r, st->cbias, st->cop, st->cclamp, st->cscale);
            og = tev_comb(A.g, B.g, C.g, D.g, st->cbias, st->cop, st->cclamp, st->cscale);
            ob = tev_comb(A.b, B.b, C.b, D.b, st->cbias, st->cop, st->cclamp, st->cscale);
        } else {
            int mode = (st->cscale << 1) | st->cop, gt;
            int av[3] = {A.r & 255, A.g & 255, A.b & 255}, bv[3] = {B.r & 255, B.g & 255, B.b & 255}, cv[3] = {C.r & 255, C.g & 255, C.b & 255};
            switch (mode >> 1) {
            case 0: gt = st->cop ? av[0] == bv[0] : av[0] > bv[0]; break;
            case 1: { int x1 = av[0] | av[1] << 8, x2 = bv[0] | bv[1] << 8; gt = st->cop ? x1 == x2 : x1 > x2; break; }
            case 2: { int x1 = av[0] | av[1] << 8 | av[2] << 16, x2 = bv[0] | bv[1] << 8 | bv[2] << 16; gt = st->cop ? x1 == x2 : x1 > x2; break; }
            default: gt = -1;
            }
            int o[3];
            for (int k = 0; k < 3; k++) { int g = gt >= 0 ? gt : (st->cop ? av[k] == bv[k] : av[k] > bv[k]); o[k] = tev_clamp((k == 0 ? D.r : k == 1 ? D.g : D.b) + (g ? cv[k] : 0), st->cclamp); }
            orr = o[0]; og = o[1]; ob = o[2];
        }
        if (st->abias != 3) oa = tev_comb(aA, aB, aC, aD, st->abias, st->aop, st->aclamp, st->ascale);
        else {
            int mode = (st->ascale << 1) | st->aop, g;
            switch (mode >> 1) {
            case 0: g = st->aop ? (A.r & 255) == (B.r & 255) : (A.r & 255) > (B.r & 255); break;
            case 1: { int x1 = (A.r & 255) | (A.g & 255) << 8, x2 = (B.r & 255) | (B.g & 255) << 8; g = st->aop ? x1 == x2 : x1 > x2; break; }
            case 2: { int x1 = (A.r & 255) | (A.g & 255) << 8 | (A.b & 255) << 16, x2 = (B.r & 255) | (B.g & 255) << 8 | (B.b & 255) << 16; g = st->aop ? x1 == x2 : x1 > x2; break; }
            default: g = st->aop ? (aA & 255) == (aB & 255) : (aA & 255) > (aB & 255); break;
            }
            oa = tev_clamp(aD + (g ? (aC & 255) : 0), st->aclamp);
        }
        reg[st->cdst].r = orr; reg[st->cdst].g = og; reg[st->cdst].b = ob;
        reg[st->adst].a = oa;
    }
    int r = clamp8(reg[0].r), g = clamp8(reg[0].g), b = clamp8(reg[0].b), a = clamp8(reg[0].a);
    if (!d->at_always) {
        int t0 = cmpf(d->at_f0, a, d->at_r0), t1 = cmpf(d->at_f1, a, d->at_r1);
        int pass = d->at_op == 0 ? (t0 && t1) : d->at_op == 1 ? (t0 || t1) : d->at_op == 2 ? (t0 ^ t1) : !(t0 ^ t1);
        if (!pass) { if (gxstats) st_px_a++; return; }
    }
    uint32_t dcol = efb[idx];
    int nr = r, ng = g, nb = b, na = a;
    if (d->blend) {
        int dr = (dcol >> 16) & 255, dg = (dcol >> 8) & 255, db = dcol & 255, dA = d->has_alpha ? (int)(dcol >> 24) : 255;
        if (d->bsub) { nr = dr - r; ng = dg - g; nb = db - b; na = dA - a; }
        else {
            int s_rgb[3] = {r, g, b}, d_rgb[3] = {dr, dg, db}, o[4];
            for (int k = 0; k < 4; k++) {
                int sv = k < 3 ? s_rgb[k] : a, dv = k < 3 ? d_rgb[k] : dA, sfac, dfac;
                switch (d->sf) { case 0: sfac = 0; break; case 1: sfac = 255; break; case 2: sfac = dv; break; case 3: sfac = 255 - dv; break;
                case 4: sfac = a; break; case 5: sfac = 255 - a; break; case 6: sfac = dA; break; default: sfac = 255 - dA; }
                switch (d->df) { case 0: dfac = 0; break; case 1: dfac = 255; break; case 2: dfac = sv; break; case 3: dfac = 255 - sv; break;
                case 4: dfac = a; break; case 5: dfac = 255 - a; break; case 6: dfac = dA; break; default: dfac = 255 - dA; }
                o[k] = (sv * sfac + dv * dfac) / 255;
            }
            nr = o[0]; ng = o[1]; nb = o[2]; na = o[3];
        }
    }
    if (d->dstconst_en) na = d->dstconst;
    uint32_t outc = dcol;
    if (d->cupd) outc = (outc & 0xFF000000u) | ((uint32_t)clamp8(nr) << 16) | ((uint32_t)clamp8(ng) << 8) | clamp8(nb);
    if (d->aupd) outc = (outc & 0x00FFFFFFu) | ((uint32_t)clamp8(na) << 24);
    efb[idx] = outc;
    if (gxstats) st_px_w++;
    if (d->zen && d->zupd) zbuf[idx] = z;
}

/* ================================================================== rasterizer */
static int scissor_l, scissor_t, scissor_r, scissor_b;
static void setup_scissor(void) {
    uint32_t tl = bp(0x20), br = bp(0x21);
    int so_x = (int)((bp(0x59) & 0x3FF) * 2) - 342, so_y = (int)(((bp(0x59) >> 10) & 0x3FF) * 2) - 342;
    scissor_t = (int)(tl & 0x7FF) - 342 - so_y; scissor_l = (int)((tl >> 12) & 0x7FF) - 342 - so_x;
    scissor_b = (int)(br & 0x7FF) - 342 - so_y; scissor_r = (int)((br >> 12) & 0x7FF) - 342 - so_x;
    if (scissor_l < 0) scissor_l = 0; if (scissor_t < 0) scissor_t = 0;
    if (scissor_r > EFB_W - 1) scissor_r = EFB_W - 1; if (scissor_b > EFB_H - 1) scissor_b = EFB_H - 1;
}

static const drawstate *cur_ds;
static void plane(float out[3], const vtx *a, const vtx *b, const vtx *c, float va, float vb, float vc, float inv) {
    /* attribute plane through 3 screen points: v = A*x + B*y + C */
    float dx1 = b->sx - a->sx, dy1 = b->sy - a->sy, dx2 = c->sx - a->sx, dy2 = c->sy - a->sy;
    float dv1 = vb - va, dv2 = vc - va;
    out[0] = (dv1 * dy2 - dv2 * dy1) * inv;
    out[1] = (dv2 * dx1 - dv1 * dx2) * inv;
    out[2] = va - out[0] * a->sx - out[1] * a->sy;
}
static void setup_tri(const vtx *a, const vtx *b, const vtx *c) {
    float area = (b->sx - a->sx) * (c->sy - a->sy) - (b->sy - a->sy) * (c->sx - a->sx);
    if (area == 0 || area != area) { if (gxstats) st_tri_zero++; return; }
    int cull = (bp(0x00) >> 14) & 3;
    if (cull == 3 || (cull == 1 && area < 0) || (cull == 2 && area > 0)) { if (gxstats) st_tri_cull++; return; }
    int minx = (int)floorf(fminf(a->sx, fminf(b->sx, c->sx))), maxx = (int)ceilf(fmaxf(a->sx, fmaxf(b->sx, c->sx)));
    int miny = (int)floorf(fminf(a->sy, fminf(b->sy, c->sy))), maxy = (int)ceilf(fmaxf(a->sy, fmaxf(b->sy, c->sy)));
    if (minx < scissor_l) minx = scissor_l; if (miny < scissor_t) miny = scissor_t;
    if (maxx > scissor_r) maxx = scissor_r; if (maxy > scissor_b) maxy = scissor_b;
    if (minx > maxx || miny > maxy) { if (gxstats) st_tri_sc++; return; }
    if (gxstats) { st_tri_ok++; if (!(a->w == b->w && b->w == c->w)) st_persp++; }
    if (ntris == captris) { captris = captris ? captris * 2 : 4096; tris = realloc(tris, sizeof(tri_t) * captris); }
    tri_t *t = &tris[ntris++];
    t->ds = cur_ds; t->minx = minx; t->maxx = maxx; t->miny = miny; t->maxy = maxy;
    /* edges, oriented so that inside is positive */
    const vtx *v[3] = {a, b, c};
    float sgn = area > 0 ? 1.0f : -1.0f;
    for (int i = 0; i < 3; i++) {
        const vtx *p = v[(i + 1) % 3], *q = v[(i + 2) % 3];
        t->e[i][0] = (p->sy - q->sy) * sgn; t->e[i][1] = (q->sx - p->sx) * sgn; t->e[i][2] = (p->sx * q->sy - q->sx * p->sy) * sgn;
        t->tl[i] = t->e[i][0] > 0 || (t->e[i][0] == 0 && t->e[i][1] > 0);
    }
    float inv = 1.0f / area;
    int persp = !(a->w == b->w && b->w == c->w);
    t->persp = persp;
    float wa = persp ? 1 / a->w : 1, wb = persp ? 1 / b->w : 1, wc = persp ? 1 / c->w : 1;
    int n = 0;
    plane(t->pa[n++], a, b, c, a->sz, b->sz, c->sz, inv);
    plane(t->pa[n++], a, b, c, wa, wb, wc, inv);
    for (int ch = 0; ch < 2; ch++) for (int k = 0; k < 4; k++)
        plane(t->pa[n++], a, b, c, a->col[ch][k] * wa, b->col[ch][k] * wb, c->col[ch][k] * wc, inv);
    for (int tc = 0; tc < cur_ds->ntex && tc < 8; tc++) for (int k = 0; k < 3; k++)
        plane(t->pa[n++], a, b, c, a->tex[tc][k] * wa, b->tex[tc][k] * wb, c->tex[tc][k] * wc, inv);
    t->nattr = n;
}

static void raster_rows(const tri_t *t, int y0, int y1) {
    const drawstate *d = t->ds;
    if (y0 < t->miny) y0 = t->miny; if (y1 > t->maxy) y1 = t->maxy;
    int ntex = d->ntex > 8 ? 8 : d->ntex;
    for (int y = y0; y <= y1; y++) {
        float py = y + 0.5f;
        float px0 = t->minx + 0.5f;
        float e0 = t->e[0][0] * px0 + t->e[0][1] * py + t->e[0][2];
        float e1 = t->e[1][0] * px0 + t->e[1][1] * py + t->e[1][2];
        float e2 = t->e[2][0] * px0 + t->e[2][1] * py + t->e[2][2];
        float at[40];
        for (int i = 0; i < t->nattr; i++) at[i] = t->pa[i][0] * px0 + t->pa[i][1] * py + t->pa[i][2];
        for (int x = t->minx; x <= t->maxx; x++) {
            if ((e0 > 0 || (e0 == 0 && t->tl[0])) && (e1 > 0 || (e1 == 0 && t->tl[1])) && (e2 > 0 || (e2 == 0 && t->tl[2]))) {
                float col[8], tex[8][2];
                float w = t->persp ? 1.0f / at[1] : 1.0f;
                for (int k = 0; k < 8; k++) col[k] = at[2 + k] * w;
                for (int tc = 0; tc < ntex; tc++) {
                    float s = at[10 + tc * 3], tt = at[11 + tc * 3], q = at[12 + tc * 3];
                    if (q != 0) { s /= q; tt /= q; }
                    tex[tc][0] = s; tex[tc][1] = tt;
                }
                float zf = at[0];
                uint32_t z = zf < 0 ? 0 : zf > 16777215.0f ? 16777215u : (uint32_t)zf;
                shade(d, (uint32_t)y * EFB_W + x, col, tex, z);
            }
            e0 += t->e[0][0]; e1 += t->e[1][0]; e2 += t->e[2][0];
            for (int i = 0; i < t->nattr; i++) at[i] += t->pa[i][0];
        }
    }
}

#define BAND 16
static void flush_band(int band, void *arg) {
    (void)arg;
    int y0 = band * BAND, y1 = y0 + BAND - 1;
    for (int i = 0; i < ntris; i++) {
        const tri_t *t = &tris[i];
        if (t->maxy < y0 || t->miny > y1) continue;
        raster_rows(t, y0, y1);
    }
}
static void gl_flush(void);
void gx_render_flush(void) {
    if (gl_on) gl_flush();
    if (gl_on && !gl_cmp) { for (int i = 0; i < npending; i++) free(pending_free[i]); npending = 0; return; }
    if (ntris) plat_parallel_for((EFB_H + BAND - 1) / BAND, flush_band, NULL);
    ntris = 0; nds = 0;
    for (int i = 0; i < npending; i++) free(pending_free[i]);
    npending = 0;
}

/* clip against the near plane (w > 0) */
static void clip_and_setup(const vtx *a, const vtx *b, const vtx *c) {
    if (gxstats) { st_tri_in++; if (a->w <= 1e-5f && b->w <= 1e-5f && c->w <= 1e-5f) st_tri_clip++; }
    if (a->w > 1e-5f && b->w > 1e-5f && c->w > 1e-5f) {
        vtx p[3] = {*a, *b, *c};
        for (int i = 0; i < 3; i++) to_screen(&p[i]);
        setup_tri(&p[0], &p[1], &p[2]);
        return;
    }
    const vtx *in[3] = {a, b, c};
    vtx poly[6]; int n = 0;
    const float eps = 1e-5f;
    for (int i = 0; i < 3; i++) {
        const vtx *p = in[i], *q = in[(i + 1) % 3];
        float dp = p->w - eps, dq = q->w - eps;
        if (dp >= 0) poly[n++] = *p;
        if ((dp >= 0) != (dq >= 0)) {
            float t = dp / (dp - dq);
            vtx v = *p;
            v.x = p->x + (q->x - p->x) * t; v.y = p->y + (q->y - p->y) * t; v.z = p->z + (q->z - p->z) * t; v.w = p->w + (q->w - p->w) * t;
            for (int ch = 0; ch < 2; ch++) for (int k = 0; k < 4; k++) v.col[ch][k] = p->col[ch][k] + (q->col[ch][k] - p->col[ch][k]) * t;
            for (int tx = 0; tx < 8; tx++) for (int k = 0; k < 3; k++) v.tex[tx][k] = p->tex[tx][k] + (q->tex[tx][k] - p->tex[tx][k]) * t;
            poly[n++] = v;
        }
    }
    if (n < 3) return;
    for (int i = 0; i < n; i++) to_screen(&poly[i]);
    for (int i = 1; i + 1 < n; i++) setup_tri(&poly[0], &poly[i], &poly[i + 1]);
}

static void gl_draw(int prim, int vat, uint32_t count, const uint8_t *data);
static const uint32_t *gl_read_efb(int sx, int sy, int w, int h);
static void gl_clear(int sx, int sy, int w, int h);
static int gl_gpucopy_on(void);
static void gl_set_scale(int s);
static void gl_overlay_efb(int sx, int sy, int w, int h);
void gx_set_render_scale(int s) { if (gl_on) gl_set_scale(s); }
static void gl_set_filter(int mode);
void gx_set_filter(int mode) { if (gl_on) gl_set_filter(mode); }
static int gl_present(int sx, int sy, int w, int h);
static void gl_copy_to_texture(int sx, int sy, int w, int h, uint32_t dest, int fmt, int intensity, int half);
static int gl_copy_into_texture(int sx, int sy, int w, int h, uint32_t dest, int fmt, int stride_bytes);
static void gx_render_draw_impl(int prim, int vat, uint32_t count, const uint8_t *data);
void gx_render_draw(int prim, int vat, uint32_t count, const uint8_t *data) {
    uint64_t t = rt_now_ns();
    g_perf_drawcount++;
    gx_render_draw_impl(prim, vat, count, data);
    g_perf_draw_ns += rt_now_ns() - t;
}
static void gx_render_draw_impl(int prim, int vat, uint32_t count, const uint8_t *data) {
    if (!count) return;
    if (gl_on) { gl_draw(prim, vat, count, data); if (!gl_cmp) return; frame_draws--; }
    static int drawcmp_frame = -2; static uint64_t prev_diff;
    if (drawcmp_frame == -2) { const char *e = getenv("RT_GLDRAWCMP"); drawcmp_frame = e ? atoi(e) : -1; }
    int drawcmp = gl_cmp && drawcmp_frame >= 0 && g_frames_presented == (uint64_t)drawcmp_frame;
    if (gxstats < 0) gxstats = getenv("RT_GXSTATS") != NULL;
    frame_draws++;
    cur_ds = build_drawstate();
    setup_scissor();
    static vtx *v; static uint32_t vcap;
    if (count > vcap) { vcap = count * 2; v = realloc(v, sizeof(vtx) * vcap); }
    const uint8_t *p = data;
    static int dump_left = -1; static uint64_t dump_frame;
    if (dump_left < 0) { const char *e = getenv("RT_GXDUMP"); dump_left = 0; if (e) { dump_frame = strtoull(e, NULL, 10); dump_left = 6; } }
    int dump = dump_left > 0 && g_frames_presented >= dump_frame && count >= 3 && prim != 0x00 && frame_draws % 50 == 0;
    if (dump) {
        dump_left--;
        rt_log("gxd", "draw prim %02X vat %d count %u vcd %08X %08X vat %08X %08X %08X matidx %08X %08X xfmat %08X %08X proj %s",
               prim, vat, count, gx.cp_vcd_lo, gx.cp_vcd_hi, gx.cp_vat_a[vat], gx.cp_vat_b[vat], gx.cp_vat_c[vat], gx.cp_matidx_a, gx.cp_matidx_b,
               gx.xf_regs[0x18], gx.xf_regs[0x19], xf_r(0x26) ? "ortho" : "persp");
        rt_log("gxd", "  proj %g %g %g %g %g %g  vp %g %g %g %g %g %g", xf_f(0x1020), xf_f(0x1021), xf_f(0x1022), xf_f(0x1023), xf_f(0x1024), xf_f(0x1025),
               xf_f(0x101A), xf_f(0x101B), xf_f(0x101C), xf_f(0x101D), xf_f(0x101E), xf_f(0x101F));
        rt_log("gxd", "  arrays pos %08X/%u nrm %08X/%u", gx.cp_array_base[0], gx.cp_array_stride[0], gx.cp_array_base[1], gx.cp_array_stride[1]);
        char hex[200]; int n = 0; for (int i = 0; i < 48 && n < 190; i++) n += snprintf(hex + n, sizeof hex - n, "%02X", data[i]);
        rt_log("gxd", "  data %s", hex);
    }
    for (uint32_t i = 0; i < count; i++) {
        rawv r; decode_vertex(vat, &p, &r); transform_vertex(&r, &v[i]);
        if (dump && i < 4) {
            uint32_t m = (uint32_t)r.pnidx * 4;
            rt_log("gxd", "  v%u pnidx %u pos %g %g %g -> clip %g %g %g w %g | mtx %g %g %g %g / %g %g %g %g / %g %g %g %g", i, r.pnidx, r.pos[0], r.pos[1], r.pos[2],
                   v[i].x, v[i].y, v[i].z, v[i].w, xf_f(m), xf_f(m + 1), xf_f(m + 2), xf_f(m + 3), xf_f(m + 4), xf_f(m + 5), xf_f(m + 6), xf_f(m + 7),
                   xf_f(m + 8), xf_f(m + 9), xf_f(m + 10), xf_f(m + 11));
        }
    }
    switch (prim) {
    case 0x00: case 0x08:
        for (uint32_t i = 0; i + 3 < count; i += 4) { clip_and_setup(&v[i], &v[i + 1], &v[i + 2]); clip_and_setup(&v[i], &v[i + 2], &v[i + 3]); }
        break;
    case 0x10: for (uint32_t i = 0; i + 2 < count; i += 3) clip_and_setup(&v[i], &v[i + 1], &v[i + 2]); break;
    case 0x18: for (uint32_t i = 0; i + 2 < count; i++) { if (i & 1) clip_and_setup(&v[i + 1], &v[i], &v[i + 2]); else clip_and_setup(&v[i], &v[i + 1], &v[i + 2]); } break;
    case 0x20: for (uint32_t i = 1; i + 1 < count; i++) clip_and_setup(&v[0], &v[i], &v[i + 1]); break;
    default: break;
    }
    if (drawcmp) {
        gx_render_flush();
        const uint32_t *G2 = gl_read_efb(0, 0, EFB_W, EFB_H);
        uint64_t diff = 0; int fx = -1, fy = -1;
        for (int i = 0; i < EFB_W * EFB_H; i++) {
            uint32_t a = efb[i], b = G2[i];
            int d = abs((int)(a >> 16 & 255) - (int)(b >> 16 & 255)) + abs((int)(a >> 8 & 255) - (int)(b >> 8 & 255)) + abs((int)(a & 255) - (int)(b & 255));
            if (d > 24) { if (fx < 0) { fx = i % EFB_W; fy = i / EFB_W; } diff++; }
        }
        if (fx >= 0 && (diff > prev_diff + 50))
            rt_log("drawcmp", "  first diff at %d,%d: sw %08X gl %08X | konst0 %d %d %d %d ksel0 %06X pe %06X", fx, fy, efb[fy * EFB_W + fx], G2[fy * EFB_W + fx],
                   tev_konst[0].r, tev_konst[0].g, tev_konst[0].b, tev_konst[0].a, bp(0xF6), bp(0x43));
        if (diff > prev_diff + 50 || diff + 50 < prev_diff) {
            uint32_t gen = bp(0x00);
            rt_log("drawcmp", "draw %u prim %02X n %u: diff %llu -> %llu | gen %06X tev0 %06X/%06X cm %06X zm %06X ac %06X tref %06X",
                   frame_draws, prim, count, (unsigned long long)prev_diff, (unsigned long long)diff, gen,
                   bp(0xC0), bp(0xC1), bp(0x41), bp(0x40), bp(0xF3), bp(0x28));
        }
        prev_diff = diff;
    }
}

/* ================================================================== EFB copies */
static void write_tex_texel(uint8_t *dst, int fmt, int x, int y, int w, uint32_t c, int intensity) {
    static const int bw[16] = {8, 8, 8, 4, 4, 4, 4, 8, 8, 8, 4, 4, 4, 0, 0, 0};
    static const int bh[16] = {8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0};
    int tw = bw[fmt], th = bh[fmt]; if (!tw) return;
    int bpb = (fmt == 6) ? 64 : 32;
    int blocks_x = (w + tw - 1) / tw;
    uint8_t *blk = dst + ((y / th) * blocks_x + (x / tw)) * bpb;
    int lx = x % tw, ly = y % th, i = ly * tw + lx;
    uint8_t r = c >> 16, g = c >> 8, b = c, a = c >> 24;
    uint8_t lum = intensity ? (uint8_t)((r * 66 + g * 129 + b * 25 + 128) / 256 + 16) : r;
    switch (fmt) {
    case 0: { uint8_t v = lum >> 4; if (lx & 1) blk[i / 2] = (blk[i / 2] & 0xF0) | v; else blk[i / 2] = (blk[i / 2] & 0x0F) | (v << 4); break; }
    case 1: blk[i] = lum; break;
    case 2: blk[i] = (a & 0xF0) | (lum >> 4); break;
    case 3: blk[i * 2] = a; blk[i * 2 + 1] = lum; break;
    case 4: { uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); blk[i * 2] = v >> 8; blk[i * 2 + 1] = (uint8_t)v; break; }
    case 5: { uint16_t v = a >= 0xE0 ? (uint16_t)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))
                                     : (uint16_t)(((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
              blk[i * 2] = v >> 8; blk[i * 2 + 1] = (uint8_t)v; break; }
    case 6: blk[i * 2] = a; blk[i * 2 + 1] = r; blk[32 + i * 2] = g; blk[32 + i * 2 + 1] = b; break;
    case 7: blk[i] = a; break;
    case 8: blk[i] = r; break;
    case 9: blk[i] = g; break;
    case 10: blk[i] = b; break;
    case 11: blk[i * 2] = g; blk[i * 2 + 1] = r; break;
    case 12: blk[i * 2] = b; blk[i * 2 + 1] = g; break;
    }
}

static void gx_render_efb_copy_impl(uint32_t val);
void gx_render_efb_copy(uint32_t val) {
    uint64_t t = rt_now_ns(), p0 = g_perf_present_ns;
    gx_render_efb_copy_impl(val);
    g_perf_copy_ns += (rt_now_ns() - t) - (g_perf_present_ns - p0);
}
static void gx_render_efb_copy_impl(uint32_t val) {
    gx_render_flush();
    uint32_t tl = bp(0x49), wh = bp(0x4A);
    int sx = tl & 0x3FF, sy = (tl >> 10) & 0x3FF, w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
    if (sx + w > EFB_W) w = EFB_W - sx; if (sy + h > EFB_H) h = EFB_H - sy;
    uint32_t dest = (bp(0x4B) & 0xFFFFFF) << 5;
    int gpu_copy = gl_on && !gl_cmp && !(val & (1u << 14)) && gl_gpucopy_on();   /* EFB->texture stays on the GPU */
    int gl_shown = 0;
    if (gl_on && !gl_cmp && (val & (1u << 14))) {               /* present straight from the GPU if the platform can */
        uint64_t t = rt_now_ns(); gl_shown = gl_present(sx, sy, w, h); if (gl_shown) g_perf_present_ns += rt_now_ns() - t;
    }
    if (gl_on && !gl_cmp && !gl_shown && (val & (1u << 14))) gl_overlay_efb(sx, sy, w, h);   /* menus into the read-back frame */
    const uint32_t *E = gl_on && !gl_cmp && !gpu_copy && !gl_shown ? gl_read_efb(sx, sy, w, h) : efb;
    if (gl_cmp && (val & (1u << 14))) {                       /* compare GL against software for this frame */
        const uint32_t *G2 = gl_read_efb(sx, sy, w, h);
        uint64_t diff = 0;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint32_t a = efb[(sy + y) * EFB_W + sx + x], b = G2[(sy + y) * EFB_W + sx + x];
            int d = abs((int)(a >> 16 & 255) - (int)(b >> 16 & 255)) + abs((int)(a >> 8 & 255) - (int)(b >> 8 & 255)) + abs((int)(a & 255) - (int)(b & 255));
            if (d > 24) diff++;
        }
        static int nframe; nframe++;
        const char *every = getenv("RT_DUMP_EVERY"); int n = every ? atoi(every) : 60;
        if (diff > (uint64_t)w * h / 200 || (n > 0 && nframe % n == 0))
            rt_log("cmp", "frame %d: %llu of %d pixels differ", nframe, (unsigned long long)diff, w * h);
        if (n > 0 && nframe % n == 0) {
            const char *dd = getenv("RT_DUMP_DIR"); char pth[1200];
            if (dd) snprintf(pth, sizeof pth, "%s/gl_%05d.ppm", dd, nframe); else snprintf(pth, sizeof pth, "%s/frames/gl_%05d.ppm", g_root, nframe);
            FILE *f = fopen(pth, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h);
                     for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint32_t c = G2[(sy + y) * EFB_W + sx + x]; fputc(c >> 16 & 255, f); fputc(c >> 8 & 255, f); fputc(c & 255, f); }
                     fclose(f); }
        }
    }
    if (val & (1u << 14)) {                                     /* to XFB: present */
        if (!gl_shown) for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint32_t c = E[(sy + y) * EFB_W + sx + x];
            present_buf[y * w + x] = 0xFF000000u | (c & 0xFFFFFF);
        }
        g_frames_presented++;
        {   /* slow frames: where did the time go? (frames over 18 ms are logged, at most 400) */
            extern uint64_t g_disc_ns;
            static uint64_t last, d0, c0, k0, dc0, cp0; static int logged;
            uint64_t now = rt_now_ns();
            if (last && now - last > 18000000ull && logged < 400 && g_frames_presented > 120) {
                logged++;
                rt_log("slow", "frame %llu took %.1f ms: draws %.1f, texture decode %.1f, shader compile %.1f, disc %.1f, efb copies %.1f",
                       (unsigned long long)g_frames_presented, (now - last) / 1e6, (g_perf_draw_ns >= d0 ? g_perf_draw_ns - d0 : g_perf_draw_ns) / 1e6, (g_decode_ns - k0) / 1e6,
                       (g_compile_ns - c0) / 1e6, (g_disc_ns - dc0) / 1e6, (g_perf_copy_ns >= cp0 ? g_perf_copy_ns - cp0 : g_perf_copy_ns) / 1e6);
            }
            last = now; d0 = g_perf_draw_ns; c0 = g_compile_ns; k0 = g_decode_ns; dc0 = g_disc_ns; cp0 = g_perf_copy_ns;
        }
        if (g_frames_presented <= 3 || g_frames_presented % 300 == 0) {
            rt_log("gx", "frame %llu: %u draws", (unsigned long long)g_frames_presented, frame_draws);
            { extern void rt_state_scan(void); const char *ss = getenv("RT_STATESCAN");
              if (ss && g_frames_presented >= strtoull(ss, NULL, 10)) rt_state_scan(); }   /* repeats each logged frame */
            { extern void rt_ax_scan(void); const char *as = getenv("RT_AXSCAN");
              if (as && g_frames_presented >= strtoull(as, NULL, 10)) { static int done; if (!done++) rt_ax_scan(); } }
            const char *pk = getenv("RT_PEEK");             /* RT_PEEK="80xxxxxx,..." words to print with each frame log */
            while (pk && *pk) {
                uint32_t a = (uint32_t)strtoul(pk, (char **)&pk, 16);
                rt_log("peek", "%08X = %08X", a, R32(a));
                if (*pk == ',') pk++; else break;
            }
            if (gxstats > 0)
                rt_log("gx", "  tris in %llu, behind %llu, zero-area %llu, culled %llu, offscreen %llu, queued %llu (persp %llu); px %llu, z-fail %llu, a-fail %llu, written %llu",
                       (unsigned long long)st_tri_in, (unsigned long long)st_tri_clip, (unsigned long long)st_tri_zero, (unsigned long long)st_tri_cull,
                       (unsigned long long)st_tri_sc, (unsigned long long)st_tri_ok, (unsigned long long)st_persp, (unsigned long long)st_px,
                       (unsigned long long)st_px_z, (unsigned long long)st_px_a, (unsigned long long)st_px_w);
        }
        st_tri_in = st_tri_clip = st_tri_zero = st_tri_cull = st_tri_sc = st_tri_ok = st_persp = st_px = st_px_z = st_px_a = st_px_w = 0;
        if (!gl_shown) { uint64_t t = rt_now_ns(); plat_present(present_buf, w, h); g_perf_present_ns += rt_now_ns() - t; }
        if (g_frames_presented % 300 == 0) perf_report();
        frame_draws = 0;
    } else {                                                    /* to texture */
        int fmtraw = (val >> 3) & 0xF;
        int fmt = fmtraw / 2 + (fmtraw & 1) * 8;
        int intensity = (val >> 15) & 1, half = (val >> 9) & 1;
        int dw = half ? w / 2 : w, dh = half ? h / 2 : h;
        uint8_t *dst = g_base + (dest & 0x3FFFFFFF);
        { static int cs = -1; if (cs < 0) cs = getenv("RT_COPYSTATS") != NULL;          /* log each distinct copy kind once */
          static uint32_t seen[256]; static int nseen;
          uint32_t key = (uint32_t)fmt | (uint32_t)intensity << 4 | (uint32_t)half << 5 | (uint32_t)w << 6 | (uint32_t)h << 18;
          int f = 0; for (int i = 0; i < nseen; i++) if (seen[i] == key) f = 1;
          g_copystats = cs; { int k = 0; for (; k < 64; k++) if (g_copydst[k] == dest) break; if (k == 64) { static int r; g_copydst[r++ & 63] = dest; } }
          if (cs && !f && nseen < 256) { seen[nseen++] = key; rt_log("copy", "frame %llu: %dx%d at %d,%d fmt %d intensity %d half %d -> %08X", (unsigned long long)g_frames_presented, w, h, sx, sy, fmt, intensity, half, dest); } }
        { static int dumped; static const char *dd; static int dd_init; if (!dd_init) { dd_init = 1; dd = getenv("RT_DUMPCOPIES"); }     /* "frame": dump that frame's EFB->texture copies */
          if (dd && g_frames_presented == strtoull(dd, NULL, 10) && dumped < 64) {
              char pth[1200]; snprintf(pth, sizeof pth, "%s/copy_%02d_%d_%d_%dx%d_fmt%d.ppm", getenv("RT_DUMP_DIR") ? getenv("RT_DUMP_DIR") : ".", dumped++, sx, sy, w, h, fmt);
              FILE *f = fopen(pth, "wb");
              if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h);
                       for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint32_t c = E[(sy + y) * EFB_W + sx + x]; fputc(c >> 16 & 255, f); fputc(c >> 8 & 255, f); fputc(c & 255, f); }
                       fclose(f); } } }
        /* destination row stride (BP 0x4D, in 32-byte units). A copy whose stride is wider than the
           copied rectangle writes into part of a larger texture (animated tiles go into the tileset
           this way), so it has to land in RAM at that stride rather than stay a GPU-side texture. */
        int vw = dw;
        { static const int cbw[16] = {8, 8, 8, 4, 4, 4, 4, 8, 8, 8, 4, 4, 4, 0, 0, 0};
          int tw = cbw[fmt & 15], bpb = (fmt == 6) ? 64 : 32;
          uint32_t stride = (bp(0x4D) & 0x3FF) * 32;
          if (tw && stride && stride != (uint32_t)((dw + tw - 1) / tw * bpb)) {
              vw = (int)(stride / bpb) * tw;
              if (gpu_copy && !half && gl_copy_into_texture(sx, sy, w, h, dest, fmt, (int)stride)) goto copied;   /* on the GPU */
              if (gpu_copy) { gpu_copy = 0; E = gl_read_efb(sx, sy, w, h); }
              tex_epoch++;                               /* textures containing it re-check their bytes */
          } }
        if (gpu_copy) gl_copy_to_texture(sx, sy, w, h, dest, fmt, intensity, half);
        else for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
            uint32_t c;
            if (half) {
                uint32_t s[4] = {E[(sy + 2 * y) * EFB_W + sx + 2 * x], E[(sy + 2 * y) * EFB_W + sx + 2 * x + 1],
                                 E[(sy + 2 * y + 1) * EFB_W + sx + 2 * x], E[(sy + 2 * y + 1) * EFB_W + sx + 2 * x + 1]};
                c = 0; for (int sh = 0; sh < 32; sh += 8) c |= (((s[0] >> sh & 255) + (s[1] >> sh & 255) + (s[2] >> sh & 255) + (s[3] >> sh & 255)) / 4) << sh;
            } else c = E[(sy + y) * EFB_W + sx + x];
            write_tex_texel(dst, fmt, x, y, vw, c, intensity);
        }
        for (int i = 0; i < TEXCACHE; i++) if (texcache[i].addr == dest) texcache[i].addr = 0xFFFFFFFF;
    copied:;
    }
    if ((val & (1u << 11)) && gl_on) gl_clear(sx, sy, w, h);
    if ((val & (1u << 11)) && (!gl_on || gl_cmp)) {                                /* clear */
        uint32_t ar = bp(0x4F), gb = bp(0x50), z = bp(0x51) & 0xFFFFFF;
        uint32_t cc = ((ar & 0xFF00) << 16) | ((ar & 0xFF) << 16) | ((gb & 0xFF00)) | (gb & 0xFF);
        uint32_t cm = bp(0x41), zm = bp(0x40);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint32_t i = (sy + y) * EFB_W + sx + x;
            uint32_t o = efb[i];
            if (cm & 8) o = (o & 0xFF000000u) | (cc & 0xFFFFFF);
            if (cm & 16) o = (o & 0x00FFFFFFu) | (cc & 0xFF000000u);
            efb[i] = o;
            if (zm & 16) zbuf[i] = z;
        }
    }
    tex_stamp++;
}

static int gl_init(void);
void gx_render_init(void) {
    gx_render_tex_invalidate();
    const char *r = getenv("NSMBW_RENDERER");
    if (r && !strcmp(r, "sw")) rt_log("gx", "software renderer (NSMBW_RENDERER=sw)");
    else if (!(gl_on = gl_init())) rt_log("gx", "OpenGL unavailable: using the software renderer");
    if (gl_on) hd_init();
    if (gl_on && getenv("RT_GLCOMPARE")) { gl_cmp = 1; rt_log("gx", "comparing OpenGL against the software renderer"); }
}
#include "rt_gxgl.inc"
