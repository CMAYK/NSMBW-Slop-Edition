/* rt_det.c - deterministic mode: what online play needs from the emulation (docs/ONLINE.md).
 *
 * With g_det on (RT_DETERMINISTIC=1, or an online session) the emulated hardware runs on a
 * virtual clock (rt_core.c) and everything the game can see from outside changes only at video
 * field boundaries (det_field): the controllers' buttons are latched there, the real-time clock
 * is a fixed date. Two PCs that start the same way and get the same buttons per field then run
 * the same game, frame for frame - the base of lockstep and rollback netplay.
 *
 * Testing: RT_DET_HASH=file writes, per field, a hash of the game's memory (MEM1 + MEM2); two
 * runs with the same inputs must give the same file (tools: diff / cmp). */
#include "rt.h"
#define XXH_INLINE_ALL
#include "xxhash.h"
#if !defined(_WIN32)
#include <sys/mman.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
static void watch_hit(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;
    void *bt[24]; int n = backtrace(bt, 24);
    fprintf(stderr, "[det] write to watched page at %p (guest %08X)\n", si->si_addr, (uint32_t)((uint8_t *)si->si_addr - g_base) | 0x80000000u);
    backtrace_symbols_fd(bt, n, 2);
    mprotect((void *)((uintptr_t)si->si_addr & ~0xFFFull), 0x1000, PROT_READ | PROT_WRITE);
}
#endif

static uint32_t kb_latched RT_STATE;              /* (the game reads it until the next field: part of a state) */
static FILE *hash_out;
static int hash_init;
extern uint64_t g_vi_fields;

/* 4 KB pages of MEM1 (24 MB) and MEM2 (64 MB) that EFB copies write: not the game's state (they
   differ with the renderer and its settings), so not hashed */
#define PG_MEM1 (32u << 8)                 /* (MEM1 24 MB + the PC mod loaded above it at 0x81800000) */
#define PG_ALL  (PG_MEM1 + (64u << 8))
static uint8_t skip_pg[PG_ALL] RT_STATE;          /* (where the game has EFB copies go: same everywhere, part of a state) */
void det_renderer_writes(uint32_t phys, uint32_t bytes) {
    phys &= 0x1FFFFFFF;
    for (uint32_t a = phys & ~0xFFFu; a < phys + bytes; a += 0x1000) {
        uint32_t pg = a < (32u << 20) ? a >> 12 : (a >= 0x10000000u && a < 0x14000000u) ? PG_MEM1 + ((a - 0x10000000u) >> 12) : 0xFFFFFFFFu;
        if (pg < PG_ALL) skip_pg[pg] = 1;
    }
}
uint64_t det_state_hash(void) {
    static XXH3_state_t st; XXH3_64bits_reset(&st);
    for (uint32_t pg = 0; pg < PG_ALL; pg++) {
        if (skip_pg[pg]) continue;
        uint32_t a = pg < PG_MEM1 ? 0x80000000u + (pg << 12) : 0x90000000u + ((pg - PG_MEM1) << 12);
        XXH3_64bits_update(&st, G(a), 0x1000);
    }
    return XXH3_64bits_digest(&st);
}
void session_field(void);
/* testing save states (rt_state.c), RT_DETERMINISTIC=1 runs:
   RT_SNAP_TEST="S,L"    take a state at field S, load it back at field L (the fields after S
                         must then hash the same both times: RT_DET_HASH);
   RT_SNAP_SAVE="F:file" write a state at field F to a file; RT_SNAP_LOAD="F:file" load one at
                         field F (in another copy: it must then go on exactly like the first) */
static void snap_tests(void) {
    static int init; static long t_s = -1, t_l = -1, sv_f = -1, ld_f = -1; static char sv_p[512], ld_p[512];
    static rt_snap *snap; static int resumed;
    if (!init) {
        init = 1; const char *e;
        if ((e = getenv("RT_SNAP_TEST"))) sscanf(e, "%ld,%ld", &t_s, &t_l);
        if ((e = getenv("RT_SNAP_SAVE"))) sscanf(e, "%ld:%511s", &sv_f, sv_p);
        if ((e = getenv("RT_SNAP_LOAD"))) sscanf(e, "%ld:%511s", &ld_f, ld_p);
        snap = rt_snap_new();
    }
    long F = (long)g_vi_fields;
    if (F == t_s && !resumed) {
        uint64_t t = rt_now_ns(); int r = rt_snap_take(snap);
        if (r == 1) { resumed = 1; rt_log("state", "test: resumed at field %ld", F); void rt_snap_compare(rt_snap *); rt_snap_compare(snap); }
        else { size_t n; rt_snap_data(snap, &n); rt_log("state", "test: state at field %ld (%.1f MB, %.1f ms)%s", F, n / 1048576.0, (rt_now_ns() - t) / 1e6, r < 0 ? " FAILED" : ""); }
    }
    if (F == t_l && !resumed) { rt_log("state", "test: loading the state of field %ld", t_s); rt_snap_load(snap); }
    if (F == sv_f) {
        int r = rt_snap_take(snap);
        if (r == 0) { size_t n; const uint8_t *d = rt_snap_data(snap, &n); FILE *f = fopen(sv_p, "wb"); if (f) { fwrite(d, 1, n, f); fclose(f); } rt_log("state", "test: wrote the state of field %ld (%.1f MB)", F, n / 1048576.0); }
        else if (r == 1) rt_log("state", "test: resumed (field %ld)", F);
    }
    if (F == ld_f && !resumed) {
        FILE *f = fopen(ld_p, "rb"); if (!f) { rt_log("state", "test: no %s", ld_p); return; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) n = 0; fclose(f);
        if (!rt_snap_set(snap, d, (size_t)n)) { rt_log("state", "test: %s isn't a state of this build", ld_p); free(d); return; }
        free(d); resumed = 1;
        rt_log("state", "test: loading %s at field %ld", ld_p, F);
        rt_snap_load(snap);
    }
}
void det_field(void) {
    {   static int f = -1; if (f < 0) { f = getenv("RT_DET_UNPACED") != NULL; } if (f) { extern int g_det_fast; g_det_fast = 1; } }   /* testing: as fast as it goes */
    kb_latched = plat_buttons();
    snap_tests();
    {   /* testing: RT_RB_TEST="start:every:depth" - rollback states from field `start`, and every
           `every` fields go back `depth` and run them again (the hashes must not change: RT_DET_HASH) */
        static long st = -2, every, depth; static uint32_t resim_to, rolled; static uint64_t nrb, tns;
        if (st == -2) { const char *e = getenv("RT_RB_TEST"); st = -1; if (e) sscanf(e, "%ld:%ld:%ld", &st, &every, &depth); }
        if (st >= 0 && !session_on()) {
            uint32_t F = (uint32_t)g_vi_fields;
            if ((long)F == st && !rb_active()) rb_start();
            if (rb_active()) {
                static uint64_t t0;
                rb_save(F, 1);
                int was = g_resim;
                g_resim = (int32_t)(resim_to - F) > 0;
                if (was && !g_resim) { tns += rt_now_ns() - t0; if (++nrb % 50 == 0) { double sm; uint64_t sv; rb_stats(&sm, &sv, NULL); rt_log("state", "test: %llu rollbacks of %ld fields, %.2f ms each to run them again; keeping a state %.2f ms/field", (unsigned long long)nrb, depth, tns / 1e6 / 50, sm); tns = 0; } }
                if (!g_resim && (long)F > st + depth && ((long)F - st) % every == 0 && F != rolled) { rolled = F; resim_to = F; t0 = rt_now_ns(); rb_rollback(F - (uint32_t)depth); }
            }
        }
    }
    {   static int dp = -1; if (dp < 0) dp = getenv("RT_DIRTY_PROBE") != NULL; if (dp) { void rt_dirty_probe(void); rt_dirty_probe(); } }
    if (net_snap_pending()) net_snap_point();              /* (the online game starts here: rt_net.c) */
    if (session_on()) session_field();                      /* (online: everyone's buttons for this field - rt_net.c) */
#if !defined(_WIN32)
    {   static long at = -2; if (at == -2) { const char *e = getenv("RT_DET_WATCH_AT"); at = e ? atol(e) : -1; }   /* testing: who writes there */
        if (at >= 0 && (long)g_vi_fields == at) {
            uint32_t a = (uint32_t)strtoul(getenv("RT_DET_WATCH"), NULL, 16);
            struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = watch_hit; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL);
            mprotect(G(a), 0x10000, PROT_READ);
        } }
#endif
    {   static int jit = -1; if (jit < 0) jit = getenv("RT_DET_JITTER") != NULL;   /* testing: the PC's timing must not matter */
        if (jit) { static uint32_t r = 12345; r = r * 1103515245u + 12345u; plat_idle_wait((uint64_t)((r >> 16) % 12) * 1000000ull); } }
    if (!hash_init) { hash_init = 1; const char *p = getenv("RT_DET_HASH"); if (p) hash_out = fopen(p, "w"); }
    {   static long at = -2; if (at == -2) { const char *e = getenv("RT_DET_DUMP_AT"); at = e ? atol(e) : -1; }   /* testing: per-page hashes */
        if (at >= 0 && (long)g_vi_fields == at && getenv("RT_DET_HASH")) {
            static int pass; pass++;                        /* (a state test passes here twice: .pages, .pages2) */
            char p[1200]; snprintf(p, sizeof p, pass > 1 ? "%s.pages%d" : "%s.pages", getenv("RT_DET_HASH"), pass); FILE *f = fopen(p, "w");
            if (f) { for (uint32_t pg = 0; pg < PG_ALL; pg++) { uint32_t a = pg < PG_MEM1 ? 0x80000000u + (pg << 12) : 0x90000000u + ((pg - PG_MEM1) << 12);
                     fprintf(f, "%08X %d %016llx\n", a, skip_pg[pg], (unsigned long long)XXH3_64bits(G(a), 0x1000)); } fclose(f); }
            const char *pk = getenv("RT_DET_PEEK");
            if (pk) { uint32_t a = (uint32_t)strtoul(pk, NULL, 16); snprintf(p, sizeof p, pass > 1 ? "%s.peek%d" : "%s.peek", getenv("RT_DET_HASH"), pass); f = fopen(p, "wb"); if (f) { fwrite(G(a), 1, 0x2000, f); fclose(f); } } } }
    if (hash_out) {
        static int every = -1; if (every < 0) { const char *e = getenv("RT_DET_HASH_EVERY"); every = e ? atoi(e) : 1; if (every < 1) every = 1; }
        if (g_vi_fields % (uint64_t)every == 0) {
            fprintf(hash_out, "%llu %llu %016llx\n", (unsigned long long)g_vi_fields, (unsigned long long)g_frames_presented,
                    (unsigned long long)det_state_hash());
            fflush(hash_out);
        }
    }
}
uint32_t det_keyboard(void) { return kb_latched; }
/* the real-time clock: a fixed date (2026-01-01 00:00, seconds since 2000) - the same everywhere */
uint32_t det_rtc_secs(void) { uint32_t t = session_on() ? session_rtc() : 0; return t ? t : 820454400u; }
