/* rt_disc.c - serve reads of the game's (decrypted) data partition from the extracted files.
 * Partition-space layout: boot.bin @0, bi2.bin @0x440, apploader @0x2440, main.dol and
 * fst.bin at the offsets in boot.bin, and every FST file at its FST offset (<<2 on Wii). */
#include "rt.h"

typedef struct { uint64_t off; uint32_t size; char path[512]; FILE *fp; uint8_t *volatile mem; } disc_file;
static disc_file *files; static int nfiles;

static int cmp_off(const void *a, const void *b) {
    const disc_file *x = a, *y = b; return x->off < y->off ? -1 : x->off > y->off;
}
static void add(uint64_t off, uint32_t size, const char *path) {
    files = realloc(files, sizeof *files * (nfiles + 1));
    disc_file *f = &files[nfiles++]; memset(f, 0, sizeof *f);
    f->off = off; f->size = size; snprintf(f->path, sizeof f->path, "%s", path);
}
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static int walk(const uint8_t *fst, const char *names, uint32_t idx, uint32_t end, const char *dir) {
    while (idx < end) {
        const uint8_t *e = fst + idx * 12;
        const char *name = names + (be32(e) & 0xFFFFFF);
        char path[512]; snprintf(path, sizeof path, "%s/%s", dir, name);
        if (e[0]) { walk(fst, names, idx + 1, be32(e + 8), path); idx = be32(e + 8); }
        else { add((uint64_t)be32(e + 4) << 2, be32(e + 8), path); idx++; }
    }
    return 0;
}

/* The whole disc (a few hundred MB) is read into RAM by a background thread at startup, so the
   game's disc reads - which on the Wii run in the background while the game keeps going - are
   memory copies here instead of file reads that stall the emulated CPU. Until a file is in RAM
   it is read from disk as before. RT_NODISCRAM=1 turns it off. */
uint64_t g_disc_ns;                                          /* time spent in disc reads (perf) */
#ifdef _WIN32
#include <windows.h>
static DWORD WINAPI preload_thread(LPVOID a)
#else
#include <pthread.h>
static void *preload_thread(void *a)
#endif
{
    (void)a;
    uint64_t t0 = rt_now_ns(), total = 0;
    for (int i = 0; i < nfiles; i++) {
        disc_file *f = &files[i];
        FILE *fp = fopen(f->path, "rb"); if (!fp) continue;
        uint8_t *m = malloc(f->size ? f->size : 1);
        size_t got = m ? fread(m, 1, f->size, fp) : 0;
        fclose(fp);
        if (got != f->size) { free(m); continue; }
        __atomic_store_n(&f->mem, m, __ATOMIC_RELEASE);
        total += f->size;
    }
    rt_log("disc", "disc in RAM: %.0f MB in %.1f s", total / 1048576.0, (rt_now_ns() - t0) / 1e9);
    return 0;
}
static void disc_preload_start(void) {
    if (getenv("RT_NODISCRAM")) return;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, preload_thread, NULL, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); }
#else
    pthread_t t; if (!pthread_create(&t, NULL, preload_thread, NULL)) pthread_detach(t);
#endif
}

int disc_init(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/sys/boot.bin", g_disc);
    FILE *f = fopen(p, "rb"); if (!f) return 1;
    uint8_t boot[0x440]; if (fread(boot, 1, sizeof boot, f) != sizeof boot) { fclose(f); return 1; } fclose(f);
    snprintf(p, sizeof p, "%s/sys/fst.bin", g_disc);
    f = fopen(p, "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *fst = malloc(n); if (fread(fst, 1, n, f) != (size_t)n) { fclose(f); return 1; } fclose(f);
    char sys[1100];
    snprintf(sys, sizeof sys, "%s/sys/boot.bin", g_disc); add(0, 0x440, sys);
    snprintf(sys, sizeof sys, "%s/sys/bi2.bin", g_disc); add(0x440, 0x2000, sys);
    snprintf(sys, sizeof sys, "%s/sys/apploader.img", g_disc); add(0x2440, 0x40000, sys);
    snprintf(sys, sizeof sys, "%s/sys/main.dol", g_disc); add((uint64_t)be32(boot + 0x420) << 2, 0x400000, sys);
    snprintf(sys, sizeof sys, "%s/sys/fst.bin", g_disc); add((uint64_t)be32(boot + 0x424) << 2, (uint32_t)n, sys);
    uint32_t count = be32(fst + 8);
    char root[1100]; snprintf(root, sizeof root, "%s/files", g_disc);
    walk(fst, (const char *)fst + count * 12, 1, count, root);
    free(fst);
    qsort(files, nfiles, sizeof *files, cmp_off);
    rt_log("disc", "%d files indexed", nfiles);
    disc_preload_start();
    return 0;
}

int disc_read(uint64_t off, uint32_t dst, uint32_t len) {
    uint64_t t0 = rt_now_ns();
    uint32_t done = 0;
    while (done < len) {
        uint64_t o = off + done;
        int lo = 0, hi = nfiles;
        while (lo < hi) { int m = (lo + hi) / 2; if (files[m].off <= o) lo = m + 1; else hi = m; }
        disc_file *f = lo ? &files[lo - 1] : NULL;
        uint32_t chunk;
        if (f && o < f->off + f->size) {
            uint64_t rel = o - f->off;
            chunk = (uint32_t)((f->off + f->size) - o); if (chunk > len - done) chunk = len - done;
            uint8_t *m = __atomic_load_n(&f->mem, __ATOMIC_ACQUIRE);
            uint32_t got = 0;
            if (m) { memcpy(G(dst + done), m + rel, chunk); got = chunk; }
            else {
                if (!f->fp) f->fp = fopen(f->path, "rb");
                if (f->fp) { fseek(f->fp, (long)rel, SEEK_SET); got = (uint32_t)fread(G(dst + done), 1, chunk, f->fp); }
            }
            if (got < chunk) memset(G(dst + done + got), 0, chunk - got);
        } else {
            uint64_t next = lo < nfiles ? files[lo].off : o + (len - done);
            chunk = (uint32_t)(next - o); if (chunk > len - done) chunk = len - done; if (!chunk) chunk = len - done;
            memset(G(dst + done), 0, chunk);
        }
        done += chunk;
    }
    LOG("disc", "read %08llX +%X -> %08X", (unsigned long long)off, len, dst);
    g_disc_ns += rt_now_ns() - t0;
    return 0;
}
