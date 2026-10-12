/* rt_hdtex.c - Dolphin-compatible custom (HD) texture packs.
 *
 * Packs go in game/textures/ (any folder layout; every .png / .dds below it is indexed at start).
 * Files are named like Dolphin's "Load/Textures/<GameID>" packs:
 *   tex1_<w>x<h>[_m]_<xxh64 of the texture data>[_<xxh64 of the used palette range>|_$]_<format>.png|dds
 * (the name is computed exactly as Dolphin's TextureInfo::CalculateTextureName does). A matching
 * file is decoded on a background thread the first time the texture is used; until it is ready
 * the original texture is shown. */
#include "rt.h"
#include <dirent.h>
#include <sys/stat.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_FAIL_MSG
#include "stb_image.h"
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "rt_hdtex.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define HD_BUCKETS 65536
static hdtex *buckets[HD_BUCKETS];
static int hd_count, hd_on = -1;
volatile uint32_t g_hd_ready;           /* bumped when a texture finished loading */

static uint32_t name_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
hdtex *hd_find(const char *name) {
    for (hdtex *e = buckets[name_hash(name) & (HD_BUCKETS - 1)]; e; e = e->next) if (!strcmp(e->name, name)) return e;
    return NULL;
}
static void add_file(const char *dir, const char *fn) {
    size_t n = strlen(fn);
    if (n < 10 || strncmp(fn, "tex1_", 5)) return;
    const char *ext = fn + n - 4; int dds;
    if (!strcmp(ext, ".png") || !strcmp(ext, ".PNG")) dds = 0; else if (!strcmp(ext, ".dds") || !strcmp(ext, ".DDS")) dds = 1; else return;
    if (strstr(fn, "_mip")) return;                 /* explicit mip levels: GL generates them */
    hdtex *e = calloc(1, sizeof *e);
    e->name = malloc(n - 3); memcpy(e->name, fn, n - 4); e->name[n - 4] = 0;
    for (char *c = e->name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    hdtex *old = hd_find(e->name);
    if (old) { if (dds && !old->dds) { free(old->path); old->path = NULL; old->dds = 1; } else { free(e->name); free(e); return; } }   /* prefer .dds */
    size_t pl = strlen(dir) + n + 2; char *path = malloc(pl); snprintf(path, pl, "%s/%s", dir, fn);
    if (old) { old->path = path; free(e->name); free(e); return; }
    e->path = path; e->dds = dds;
    uint32_t b = name_hash(e->name) & (HD_BUCKETS - 1); e->next = buckets[b]; buckets[b] = e; hd_count++;
}
static void scan(const char *dir, int depth) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        size_t pl = strlen(dir) + strlen(de->d_name) + 2; char *p = malloc(pl); snprintf(p, pl, "%s/%s", dir, de->d_name);
        struct stat st;
        if (!stat(p, &st)) { if (S_ISDIR(st.st_mode)) { if (depth < 12) scan(p, depth + 1); } else add_file(dir, de->d_name); }
        free(p);
    }
    closedir(d);
}

/* ---- background loader ---- */
#define QN 4096
static hdtex *queue[QN]; static uint32_t q_head, q_tail;
#ifdef _WIN32
static CRITICAL_SECTION q_lock; static CONDITION_VARIABLE q_cv;
#define LOCK() EnterCriticalSection(&q_lock)
#define UNLOCK() LeaveCriticalSection(&q_lock)
#define WAIT() SleepConditionVariableCS(&q_cv, &q_lock, INFINITE)
#define SIGNAL() WakeConditionVariable(&q_cv)
#else
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;
#define LOCK() pthread_mutex_lock(&q_lock)
#define UNLOCK() pthread_mutex_unlock(&q_lock)
#define WAIT() pthread_cond_wait(&q_cv, &q_lock)
#define SIGNAL() pthread_cond_signal(&q_cv)
#endif

static uint8_t *read_file(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(sz > 0 ? (size_t)sz : 1); *n = fread(d, 1, (size_t)sz, f); fclose(f);
    return d;
}
static int load_dds(hdtex *e, uint8_t *d, size_t n) {
    if (n < 128 || memcmp(d, "DDS ", 4)) return 0;
    uint32_t h = *(uint32_t *)(d + 12), w = *(uint32_t *)(d + 16), mips = *(uint32_t *)(d + 28);
    uint32_t pfflags = *(uint32_t *)(d + 80), bits = *(uint32_t *)(d + 88); const char *cc = (const char *)(d + 84);
    size_t off = 128; int fmt = 0, bs = 0;
    if ((pfflags & 4) && !memcmp(cc, "DXT1", 4)) { fmt = HD_BC1; bs = 8; }
    else if ((pfflags & 4) && !memcmp(cc, "DXT3", 4)) { fmt = HD_BC2; bs = 16; }
    else if ((pfflags & 4) && !memcmp(cc, "DXT5", 4)) { fmt = HD_BC3; bs = 16; }
    else if ((pfflags & 4) && !memcmp(cc, "DX10", 4)) {
        uint32_t dxgi = *(uint32_t *)(d + 128); off = 148;
        if (dxgi == 71 || dxgi == 72) { fmt = HD_BC1; bs = 8; } else if (dxgi == 74 || dxgi == 75) { fmt = HD_BC2; bs = 16; }
        else if (dxgi == 77 || dxgi == 78) { fmt = HD_BC3; bs = 16; } else if (dxgi == 98 || dxgi == 99) { fmt = HD_BC7; bs = 16; }
        else if (dxgi == 28 || dxgi == 29) fmt = HD_RGBA8; else if (dxgi == 87 || dxgi == 91) fmt = HD_BGRA8;
    } else if (bits == 32) fmt = (*(uint32_t *)(d + 92) == 0x00FF0000) ? HD_BGRA8 : HD_RGBA8;
    if (!fmt) return 0;
    if (!mips) mips = 1; if (mips > 16) mips = 16;
    e->w = (int)w; e->h = (int)h; e->fmt = fmt; e->nlev = 0;
    uint32_t lw = w, lh = h;
    for (uint32_t i = 0; i < mips; i++) {
        size_t sz = bs ? (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * bs : (size_t)lw * lh * 4;
        if (off + sz > n) break;
        e->lev[e->nlev].off = off; e->lev[e->nlev].size = sz; e->lev[e->nlev].w = (int)lw; e->lev[e->nlev].h = (int)lh; e->nlev++;
        off += sz; lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
    }
    if (!e->nlev) return 0;
    e->data = d; return 1;
}
static void load_one(hdtex *e) {
    size_t n; uint8_t *d = read_file(e->path, &n);
    int ok = 0;
    if (d && e->dds) ok = load_dds(e, d, n);
    else if (d) {
        int w, h, c; uint8_t *px = stbi_load_from_memory(d, (int)n, &w, &h, &c, 4);
        free(d); d = NULL;
        if (px) { e->data = px; e->w = w; e->h = h; e->fmt = HD_RGBA8; e->nlev = 1; e->lev[0].off = 0; e->lev[0].size = (size_t)w * h * 4; e->lev[0].w = w; e->lev[0].h = h; ok = 1; }
    }
    if (!ok) { free(d); rt_log("hdtex", "could not load %s", e->path); }
    e->state = ok ? HD_READY : HD_FAILED;
    if (ok) g_hd_ready++;
}
#ifdef _WIN32
static DWORD WINAPI loader(LPVOID a) {
#else
static void *loader(void *a) {
#endif
    (void)a;
    for (;;) {
        LOCK();
        while (q_head == q_tail) WAIT();
        hdtex *e = queue[q_tail++ % QN];
        UNLOCK();
        load_one(e);
    }
    return 0;
}
void hd_request(hdtex *e) {
    if (e->state != HD_IDLE) return;
    LOCK();
    if (q_head - q_tail < QN) { e->state = HD_QUEUED; queue[q_head++ % QN] = e; SIGNAL(); }
    UNLOCK();
}

int hd_enabled(void) { return hd_on > 0; }
void hd_init(void) {
    if (hd_on >= 0) return;
    hd_on = 0;
    char dir[1200]; snprintf(dir, sizeof dir, "%s/textures", g_root);
    scan(dir, 0);
    if (!hd_count) { rt_log("hdtex", "no custom textures in %s", dir); return; }
    rt_log("hdtex", "%d custom textures indexed from %s", hd_count, dir);
#ifdef _WIN32
    InitializeCriticalSection(&q_lock); InitializeConditionVariable(&q_cv);
    for (int i = 0; i < 2; i++) CreateThread(NULL, 0, loader, NULL, 0, NULL);
#else
    for (int i = 0; i < 2; i++) { pthread_t t; pthread_create(&t, NULL, loader, NULL); pthread_detach(t); }
#endif
    hd_on = 1;
}

/* Dolphin's name for a texture (see TextureInfo::CalculateTextureName); data = texture bytes in RAM,
   tlut = the TMEM palette area (CI formats only) */
hdtex *hd_lookup(const uint8_t *data, size_t size, const uint8_t *tlut, size_t tlut_avail, uint32_t w, uint32_t h, uint32_t fmt, int mipmapped) {
    if (hd_on <= 0) return NULL;
    size_t tlut_size = fmt == 8 ? 32 : fmt == 9 ? 512 : fmt == 10 ? 32768 : 0;
    uint32_t mn = 0xFFFF, mx = 0;
    if (tlut_size == 32) for (size_t i = 0; i < size; i++) { uint32_t lo = data[i] & 15, hi = data[i] >> 4; if (lo < mn) mn = lo; if (hi < mn) mn = hi; if (lo > mx) mx = lo; if (hi > mx) mx = hi; }
    else if (tlut_size == 512) for (size_t i = 0; i < size; i++) { if (data[i] < mn) mn = data[i]; if (data[i] > mx) mx = data[i]; }
    else if (tlut_size == 32768) for (size_t i = 0; i < size; i += 2) { uint32_t v = ((uint32_t)data[i] << 8) & 0x3FFF; if (v < mn) mn = v; if (v > mx) mx = v; }   /* (sic: as Dolphin) */
    const uint8_t *tl = tlut;
    if (tlut_size) { tlut_size = 2 * (size_t)(mx + 1 - mn); tl += 2 * mn; if (2 * (size_t)mn + tlut_size > tlut_avail) tlut_size = 0; }
    unsigned long long th = XXH64(data, size, 0), lh = tlut_size ? XXH64(tl, tlut_size, 0) : 0;
    char name[128];
    for (int variant = 0; variant < 4; variant++) {
        int m = variant & 1 ? !mipmapped : mipmapped;           /* exact first, then the other mip flag */
        int wild = variant >= 2;                                 /* palette wildcard "$" */
        if (wild && !tlut_size) break;
        if (wild) snprintf(name, sizeof name, "tex1_%ux%u%s_%016llx_$_%u", w, h, m ? "_m" : "", th, fmt);
        else if (tlut_size) snprintf(name, sizeof name, "tex1_%ux%u%s_%016llx_%016llx_%u", w, h, m ? "_m" : "", th, lh, fmt);
        else snprintf(name, sizeof name, "tex1_%ux%u%s_%016llx_%u", w, h, m ? "_m" : "", th, fmt);
        static int log_names = -1; if (log_names < 0) log_names = getenv("RT_HDNAMES") != NULL;
        if (log_names && variant == 0) rt_log("hdtex", "lookup %s", name);
        hdtex *e = hd_find(name);
        if (e && e->state != HD_FAILED) return e;
    }
    return NULL;
}

/* a PNG (or other stb_image format) as 0xAARRGGBB pixels for the in-game menus */
uint32_t *ui_load_png(const char *path, int *w, int *h) {
    int n; unsigned char *d = stbi_load(path, w, h, &n, 4);
    if (!d) return NULL;
    uint32_t *px = malloc((size_t)*w * *h * 4);
    for (int i = 0; i < *w * *h; i++) px[i] = (uint32_t)d[i * 4 + 3] << 24 | (uint32_t)d[i * 4] << 16 | (uint32_t)d[i * 4 + 1] << 8 | d[i * 4 + 2];
    stbi_image_free(d);
    return px;
}
