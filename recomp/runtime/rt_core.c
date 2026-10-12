/* rt_core.c - boot, guest threads (as host fibers), exceptions/interrupts, timers, tracing.
 *
 * Threads: the Wii OS switches threads only through OSSaveContext / OSLoadContext.
 * We replace both: every guest OSContext that gets saved is bound to the host fiber that
 * saved it, and loading it switches to that fiber. A context that was never saved (a
 * freshly created thread) gets a new fiber that calls the entry point from the context.
 *
 * Interrupts: delivered at poll points (loop back-edges and interrupt-enable calls). The
 * guest's own handler from the OS exception table runs; when it finishes with
 * OSLoadContext(the interrupted context) we unwind back to the poll point and restore
 * the register snapshot - the equivalent of rfi.
 */
#include "rt.h"
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t *g_base;
char g_root[1024], g_disc[1024];
int g_verbose = 0;
volatile uint32_t ppc_poll_counter;
#ifdef PPC_TRACE
uint32_t ppc_trace_ring[256]; uint32_t ppc_trace_pos;
/* RT_FUNCSTAT="first,count": histogram of recently entered guest functions, sampled once per VI
 * field over that range of fields, printed at the end (debug aid for "what is the game doing"). */
static uint32_t fs_key[1 << 16], fs_cnt[1 << 16];
void rt_funcstat_tick(void) {
    static int init, first, count; static uint64_t field;
    if (!init) { init = 1; const char *e = getenv("RT_FUNCSTAT"); if (e) sscanf(e, "%d,%d", &first, &count); }
    if (!count) return;
    extern uint64_t g_frames_presented;
    field = g_frames_presented;   /* window measured in presented frames */
    if (field < (uint64_t)first) return;
    if (field < (uint64_t)(first + count)) {
        static uint64_t last_dump;
        if (field >= last_dump + 50) {                  /* print the ring's game-code entries in order */
            last_dump = field;
            fprintf(stderr, "[funcstat] ring at frame %llu:", (unsigned long long)field);
            uint32_t prevp = 0;
            for (int i = 0; i < 256; i++) {
                uint32_t a = ppc_trace_ring[(ppc_trace_pos + i) & 255];
                if (!a || (a >= 0x80190000u && a < 0x80700000u) || a == prevp || a == 0x800046B4u || a == 0x80004600u) continue;
                prevp = a; uint32_t o; const char *n = rt_func_name(a, &o);
                fprintf(stderr, " %s", n ? n : "?");
            }
            fprintf(stderr, "\n");
        }
        for (int i = 0; i < 256; i++) {
            uint32_t a = ppc_trace_ring[i]; if (!a) continue;
            if (a == 0x800046B4u) {                      /* memset: credit the previously entered function too */
                uint32_t prev = ppc_trace_ring[(i - 1) & 255], prev2 = ppc_trace_ring[(i - 2) & 255];
                static uint32_t pk[4096], pc[4096]; static int np;
                uint32_t key = prev ^ (prev2 << 1);
                int j; for (j = 0; j < np && pk[j] != key; j++) {}
                if (j == np && np < 4096) { pk[np] = key; np++; }
                if (j < 4096) pc[j]++;
                if (pc[j] == 200) { uint32_t o; const char *n1 = rt_func_name(prev, &o), *n2 = rt_func_name(prev2, &o);
                    fprintf(stderr, "[funcstat] memset after %08X %s <- %08X %s\n", prev, n1 ? n1 : "?", prev2, n2 ? n2 : "?"); }
            }
            uint32_t h = (a * 2654435761u) >> 16;
            while (fs_key[h] && fs_key[h] != a) h = (h + 1) & 0xFFFF;
            fs_key[h] = a; fs_cnt[h]++;
        }
        return;
    }
    static int printed;
    if (!printed && field >= (uint64_t)(first + count)) {
        printed = 1;
        fprintf(stderr, "[funcstat] fields %d..%d:\n", first, first + count);
        uint32_t lo = 0x80190000u, hi = 0x80700000u; int inside = 0;
        { const char *r = getenv("RT_FUNCSTAT_RANGE"); if (r) { sscanf(r, "%x,%x", &lo, &hi); inside = 1; } }
        for (int k = 0; k < 150; k++) {
            int best = -1;
            for (int i = 0; i < 65536; i++) {
                if (!fs_cnt[i]) continue;
                int in = fs_key[i] >= lo && fs_key[i] < hi;
                if (in != inside) continue;
                if (best < 0 || fs_cnt[i] > fs_cnt[best]) best = i;
            }
            if (best < 0) break;
            uint32_t off; const char *n = rt_func_name(fs_key[best], &off);
            fprintf(stderr, "  %6u %08X %s\n", fs_cnt[best], fs_key[best], n ? n : "?");
            fs_cnt[best] = 0;
        }
    }
}
#else
void rt_funcstat_tick(void) {}
#endif

/* ------------------------------------------------------------------ logging */
void rt_log(const char *cat, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%s] ", cat); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); fflush(stderr);
}
int rt_log_enabled(const char *cat) {
    static int init; static const char *f;
    if (!init) { init = 1; f = getenv("RT_LOGCAT"); }
    if (!f || !*f) return 1;
    size_t n = strlen(cat); const char *p = f;
    while ((p = strstr(p, cat))) { if ((p == f || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1; p += n; }
    return 0;
}
void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "\n*** FATAL: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    rt_dump_trace();
    fflush(stderr);
#ifdef _WIN32
    { char m[512]; va_list a2; va_start(a2, fmt); vsnprintf(m, sizeof m, fmt, a2); va_end(a2);
      MessageBoxA(NULL, m, "nsmbw: fatal error", MB_OK | MB_ICONERROR); }
#endif
    exit(1);
}

const char *rt_func_name(uint32_t addr, uint32_t *off) {
    uint32_t lo = 0, hi = ppc_func_count;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (ppc_func_names[m].addr <= addr) lo = m + 1; else hi = m; }
    if (lo == 0) return "?";
    const ppc_func_name *f = &ppc_func_names[lo - 1];
    if (off) *off = addr - f->addr;
    return addr - f->addr < f->size + 0x1000 ? f->name : "?";
}
static void backtrace_guest(uint32_t sp, uint32_t lr) {
    uint32_t off; const char *n = rt_func_name(lr, &off);
    fprintf(stderr, "      %08X %s+0x%X\n", lr, n, off);
    for (int d = 0; d < 24 && sp >= 0x80000000u && sp < 0x81800000u; d++) {
        uint32_t next = R32(sp);
        if (next <= sp || next >= 0x81800000u) break;
        uint32_t ret = R32(next + 4);
        n = rt_func_name(ret, &off);
        fprintf(stderr, "      %08X %s+0x%X\n", ret, n, off);
        sp = next;
    }
}
void rt_dump_threads(void) {
    if (!g_base) return;
    fprintf(stderr, "guest threads (current %08X):\n", R32(0x800000E4));
    uint32_t t = R32(0x800000DC);
    for (int i = 0; t && i < 32; i++, t = R32(t + 0x2FC)) {
        fprintf(stderr, "  thread %08X state %u prio %d suspend %d\n", t, R16(t + 0x2C8), (int)R32(t + 0x2D0), (int)R32(t + 0x2CC));
        backtrace_guest(R32(t + 4), R32(t + 0x84));
    }
}
void rt_dump_trace(void) {
    rt_dump_threads();
#ifdef PPC_TRACE
    fprintf(stderr, "last functions entered (oldest first):\n");
    for (int i = 0; i < 48; i++) {
        uint32_t a = ppc_trace_ring[(ppc_trace_pos - 48 + i) & 255];
        if (a) fprintf(stderr, "  %08X %s\n", a, rt_func_name(a, NULL));
    }
#endif
}

/* ------------------------------------------------------------------ settings (game/settings.ini) */
static const struct { const char *key; int def; const char *help; } setting_defs[] = {
    {"render_scale", 2, "internal resolution: 1 = original 640x528, 2 = 2x, 3 = 3x, 4 = 4x"},
    {"window_width", 0, "window size in pixels (0 = pick the largest size that fits the screen)"},
    {"window_height", 0, "(see window_width)"},
    {"fixed_window_size", 1, "1 = the window can't be resized"},
    {"fullscreen", 0, "1 = fullscreen (borderless, the size of the monitor; F11 or Alt+Enter toggles, Esc leaves it)"},
    {"texture_filter", 1, "0 = nearest (pixel perfect), 1 = bilinear, 2/3/4/5 = anisotropic 2x/4x/8x/16x"},
    {"aspect_4_3", 1, "1 = full-screen 4:3 gameplay (no letterbox), 0 = widescreen 16:9"},
    /* mkwcat's nsmbw-project (8 players) options, read when the game starts */
    {"mod_shake_with_button", 0, "8-player mod: 1 = a button does the shake/spin (no motion needed)"},
    {"mod_death_messages", 0, "8-player mod: show messages when players die: 0 = off, 1 = on all damage, 2 = only on death"},
    {"mod_infinite_lives", 0, "8-player mod: 1 = count deaths instead of lives (no game over)"},
    {"mod_no_powerup_pause", 0, "8-player mod: 1 = don't pause the game when someone powers up"},
    {"mod_always_exit_course", 0, "8-player mod: 1 = Exit Course is always available in the pause menu"},
    {"mod_hide_motion_guides", 1, "8-player mod: hide motion control speech bubbles: 0 = never, 1 = with 5+ players, 2 = always"},
    {"mod_yoshi_colors", 0, "8-player mod: Yoshi colors: 0 = normal, 1 = random, 2 = always green"},
    {"mod_unlocked_all_worlds", 0, "8-player mod: 1 = all worlds available"},
    {"mod_all_paths_open", 0, "8-player mod: 1 = all world map paths available"},
    {"mod_liftable_tiles", 0, "8-player mod: 1 = liftable tiles"},
    {"mod_bubble_swarm", 0, "8-player mod: 1 = 1-Up bubble swarm mode"},
    {"mod_deadly_1up", 0, "8-player mod: 1 = deadly 1-ups"},
};
static char setting_keys[64][64]; static int setting_vals[64], nsettings, settings_loaded;
int rt_setting(const char *key, int def) {
    if (!settings_loaded) {
        settings_loaded = 1;
        char p[1200]; snprintf(p, sizeof p, "%s/settings.ini", g_root);
        FILE *f = fopen(p, "r");
        if (!f) {                                        /* first run: write the defaults, documented */
            if ((f = fopen(p, "w"))) {
                fprintf(f, "; New Super Mario Bros. Wii native - settings\n");
                for (size_t i = 0; i < sizeof setting_defs / sizeof *setting_defs; i++) fprintf(f, "\n; %s\n%s = %d\n", setting_defs[i].help, setting_defs[i].key, setting_defs[i].def);
                fclose(f);
            }
            f = fopen(p, "r");
        }
        char line[256];
        while (f && fgets(line, sizeof line, f) && nsettings < 64) {
            char k[64]; int v;
            if (line[0] == ';' || line[0] == '#') continue;
            if (sscanf(line, " %63[A-Za-z0-9_] = %d", k, &v) == 2) { snprintf(setting_keys[nsettings], 64, "%s", k); setting_vals[nsettings++] = v; }
        }
        if (f) fclose(f);
        /* options added by newer builds: append them (documented) so they show up in the file */
        FILE *a = NULL;
        for (size_t d = 0; d < sizeof setting_defs / sizeof *setting_defs; d++) {
            int have = 0; for (int i = 0; i < nsettings; i++) if (!strcmp(setting_keys[i], setting_defs[d].key)) have = 1;
            if (have) continue;
            if (!a) a = fopen(p, "a");
            if (a) fprintf(a, "\n; %s\n%s = %d\n", setting_defs[d].help, setting_defs[d].key, setting_defs[d].def);
        }
        if (a) fclose(a);
    }
    for (int i = 0; i < nsettings; i++) if (!strcmp(setting_keys[i], key)) return setting_vals[i];
    return def;
}
/* change a setting and save it: the "key = value" line in settings.ini is rewritten in place */
void rt_setting_set(const char *key, int val) {
    rt_setting(key, 0);
    int found = 0;
    for (int i = 0; i < nsettings; i++) if (!strcmp(setting_keys[i], key)) { setting_vals[i] = val; found = 1; }
    if (!found && nsettings < 64) { snprintf(setting_keys[nsettings], 64, "%s", key); setting_vals[nsettings++] = val; }
    char p[1200]; snprintf(p, sizeof p, "%s/settings.ini", g_root);
    FILE *f = fopen(p, "r"); char *out = NULL; size_t n = 0, cap = 0; int written = 0;
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        char k[64]; int v;
        if (line[0] != ';' && sscanf(line, " %63[A-Za-z0-9_] = %d", k, &v) == 2 && !strcmp(k, key)) { snprintf(line, sizeof line, "%s = %d\n", key, val); written = 1; }
        size_t l = strlen(line);
        if (n + l + 1 > cap) { cap = (n + l + 1) * 2; out = realloc(out, cap); }
        memcpy(out + n, line, l + 1); n += l;
    }
    if (f) fclose(f);
    if (!written) { snprintf(line, sizeof line, "%s = %d\n", key, val); size_t l = strlen(line); if (n + l + 1 > cap) { cap = n + l + 1; out = realloc(out, cap); } memcpy(out + n, line, l + 1); n += l; }
    if ((f = fopen(p, "w"))) { if (out) fwrite(out, 1, n, f); fclose(f); }
    free(out);
}

/* ------------------------------------------------------------------ time */
static uint64_t t0_ns; static int64_t tb_offset;
static uint64_t os_now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}
/* The clock is read constantly (interrupt polling, timebase, perf counters), so after a 200 ms
 * calibration against the OS clock it comes straight from the CPU's time-stamp counter, which
 * costs a few ns instead of a system call / QueryPerformanceCounter. */
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
static double tsc_scale; static uint64_t tsc0, tns0; static volatile int clk_state;
uint64_t rt_now_ns(void) {
    if (clk_state == 2) return tns0 + (uint64_t)((double)(__rdtsc() - tsc0) * tsc_scale);
    uint64_t n = os_now_ns(), t = __rdtsc();
    if (!clk_state) { tsc0 = t; tns0 = n; clk_state = 1; return n; }
    if (n - tns0 >= 200000000ull && t > tsc0) { tsc_scale = (double)(n - tns0) / (double)(t - tsc0); tsc0 = t; tns0 = n; clk_state = 2; }
    return n;
}
#else
uint64_t rt_now_ns(void) { return os_now_ns(); }
#endif
uint64_t rt_tb_now(void) {
    uint64_t ns = rt_now_ns() - t0_ns;
    return (uint64_t)((int64_t)((ns / 1000000000ull) * TB_HZ + (ns % 1000000000ull) * TB_HZ / 1000000000ull) + tb_offset);
}
uint64_t ppc_timebase(void) { return rt_tb_now(); }
/* Gekko locked-cache DMA (DMA_U = SPR 922, DMA_L = SPR 923). The locked cache lives at
 * 0xE0000000, which our physical mask maps to 0x20000000 inside guest memory. NW4R G3D computes
 * model matrices there and DMAs them out, so without this every 3D model has zero matrices. */
void ppc_set_dma_l(ppc_context *ctx, uint8_t *base, uint32_t v) {
    uint32_t u = ctx->spr[922];
    if (v & 2) {                                            /* DMA_T: trigger */
        uint32_t len = ((u & 0x1F) << 2) | ((v >> 2) & 3); if (!len) len = 128;
        uint32_t mem = u & 0xFFFFFFE0u, lc = v & 0xFFFFFFE0u, n = len * 32;
        uint8_t *m = base + PPC_PHYS(mem), *c = base + PPC_PHYS(lc);
        if (v & 0x10) memcpy(c, m, n);                      /* DMA_LD: memory -> locked cache */
        else memcpy(m, c, n);                               /* store: locked cache -> memory */
    }
    ctx->spr[923] = v & ~3u;                                /* done instantly: clear T and F */
}
void ppc_set_tb(ppc_context *ctx, int upper, uint32_t v) {
    (void)ctx;
    uint64_t cur = rt_tb_now();
    uint64_t want = upper ? (((uint64_t)v << 32) | (uint32_t)cur) : ((cur & 0xFFFFFFFF00000000ull) | v);
    tb_offset += (int64_t)(want - cur);
}
static uint64_t dec_deadline; static int dec_armed;
uint32_t ppc_get_dec(ppc_context *ctx) { (void)ctx; return (uint32_t)(dec_deadline - rt_tb_now()); }
void ppc_set_dec(ppc_context *ctx, uint32_t v) {
    (void)ctx;
    dec_deadline = rt_tb_now() + (int64_t)(int32_t)v;
    dec_armed = 1;
}

/* ------------------------------------------------------------------ function lookup */
static ppc_func lookup(uint32_t addr) {
    uint32_t lo = 0, hi = ppc_func_count;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (ppc_func_table[m].addr == addr) return ppc_func_table[m].fn;
        if (ppc_func_table[m].addr < addr) lo = m + 1; else hi = m;
    }
    return NULL;
}
void ppc_call_indirect(ppc_context *ctx, uint8_t *base, uint32_t addr) {
    ppc_func f = lookup(addr);
    if (!f) {
        uint32_t off = 0; const char *n = rt_func_name(addr, &off);
        rt_fatal("call to %08X (%s+0x%X) which is not a known function start; lr=%08X", addr, n, off, ctx->lr);
    }
    f(ctx, base);
}
void ppc_unimplemented(ppc_context *ctx, uint8_t *base, uint32_t addr, const char *what) {
    (void)ctx; (void)base; rt_fatal("unimplemented instruction %s at %08X", what, addr);
}
void ppc_syscall(ppc_context *ctx, uint8_t *base, uint32_t addr) { (void)ctx; (void)base; static int n; if (n < 4) { n++; LOG("cpu", "sc at %08X (ignored)", addr); } }
void ppc_trap(ppc_context *ctx, uint8_t *base, uint32_t addr) { (void)base; rt_fatal("trap at %08X lr=%08X", addr, ctx->lr); }

/* ------------------------------------------------------------------ non-local jumps
 * Windows x64: clang's __builtin_setjmp/longjmp does not treat RSI/RDI/XMM6-15 (callee-saved
 * on Win64 only) as clobbered, so interrupted code got corrupted registers. Use a tiny
 * hand-written setjmp/longjmp there that saves every Win64 non-volatile register. */
#ifdef _WIN32
typedef struct { uint64_t gpr[10]; uint8_t xmm[10][16]; } rt_jmpbuf;   /* rbx rbp rdi rsi r12-r15 rsp rip */
int rt_setjmp(rt_jmpbuf *b) __attribute__((returns_twice));
void rt_longjmp(rt_jmpbuf *b, int v) __attribute__((noreturn));
__asm__(
    ".text\n.globl rt_setjmp\nrt_setjmp:\n"
    "  movq %rbx, 0(%rcx)\n  movq %rbp, 8(%rcx)\n  movq %rdi, 16(%rcx)\n  movq %rsi, 24(%rcx)\n"
    "  movq %r12, 32(%rcx)\n  movq %r13, 40(%rcx)\n  movq %r14, 48(%rcx)\n  movq %r15, 56(%rcx)\n"
    "  leaq 8(%rsp), %rdx\n  movq %rdx, 64(%rcx)\n  movq (%rsp), %rdx\n  movq %rdx, 72(%rcx)\n"
    "  movdqu %xmm6, 80(%rcx)\n  movdqu %xmm7, 96(%rcx)\n  movdqu %xmm8, 112(%rcx)\n  movdqu %xmm9, 128(%rcx)\n"
    "  movdqu %xmm10, 144(%rcx)\n  movdqu %xmm11, 160(%rcx)\n  movdqu %xmm12, 176(%rcx)\n  movdqu %xmm13, 192(%rcx)\n"
    "  movdqu %xmm14, 208(%rcx)\n  movdqu %xmm15, 224(%rcx)\n"
    "  xorl %eax, %eax\n  ret\n"
    ".globl rt_longjmp\nrt_longjmp:\n"
    "  movq 0(%rcx), %rbx\n  movq 8(%rcx), %rbp\n  movq 16(%rcx), %rdi\n  movq 24(%rcx), %rsi\n"
    "  movq 32(%rcx), %r12\n  movq 40(%rcx), %r13\n  movq 48(%rcx), %r14\n  movq 56(%rcx), %r15\n"
    "  movdqu 80(%rcx), %xmm6\n  movdqu 96(%rcx), %xmm7\n  movdqu 112(%rcx), %xmm8\n  movdqu 128(%rcx), %xmm9\n"
    "  movdqu 144(%rcx), %xmm10\n  movdqu 160(%rcx), %xmm11\n  movdqu 176(%rcx), %xmm12\n  movdqu 192(%rcx), %xmm13\n"
    "  movdqu 208(%rcx), %xmm14\n  movdqu 224(%rcx), %xmm15\n"
    "  movl %edx, %eax\n  movq 64(%rcx), %rsp\n  jmpq *72(%rcx)\n");
#define RT_JMPBUF rt_jmpbuf
#define RT_SETJMP(b) rt_setjmp(&(b))
#define RT_LONGJMP(b) rt_longjmp(&(b), 1)
#else
typedef void *rt_jmpbuf_gnu[5];
#define RT_JMPBUF rt_jmpbuf_gnu
#define RT_SETJMP(b) __builtin_setjmp(b)
#define RT_LONGJMP(b) __builtin_longjmp(b, 1)
#endif

/* ------------------------------------------------------------------ threads */
#define MAX_POLL 16
typedef struct {
    uint32_t exc_ctx;               /* guest context the interrupt was taken in */
    RT_JMPBUF jb;                   /* setjmp buffer */
    ppc_context *ctx;               /* everything needed after the jump lives here, not in */
    ppc_context snap;               /* registers: Win64 callee-saved regs aren't restored  */
} poll_frame;

typedef struct guest_thread {
    rt_fiber *fiber;
    ppc_context ctx;
    poll_frame polls[MAX_POLL];
    int npoll;
    uint32_t entry;
    int started;
} guest_thread;

#define MAX_BIND 256
static struct { uint32_t gctx, srr0; guest_thread *t; } binds[MAX_BIND];
static int nbinds;
static guest_thread main_thread, *cur;

static void ctx_bind(uint32_t gctx, uint32_t srr0, guest_thread *t) {
    for (int i = 0; i < nbinds; i++)
        if (binds[i].gctx == gctx) { binds[i].srr0 = srr0; binds[i].t = t; return; }
    if (nbinds == MAX_BIND) rt_fatal("too many thread contexts");
    binds[nbinds].gctx = gctx; binds[nbinds].srr0 = srr0; binds[nbinds].t = t; nbinds++;
}
static guest_thread *ctx_bound(uint32_t gctx, uint32_t *srr0) {
    for (int i = 0; i < nbinds; i++) if (binds[i].gctx == gctx) { *srr0 = binds[i].srr0; return binds[i].t; }
    return NULL;
}

/* OSContext layout */
enum { OSC_GPR = 0x0, OSC_CR = 0x80, OSC_LR = 0x84, OSC_CTR = 0x88, OSC_XER = 0x8C, OSC_FPR = 0x90,
       OSC_FPSCR = 0x194, OSC_SRR0 = 0x198, OSC_SRR1 = 0x19C, OSC_MODE = 0x1A0, OSC_STATE = 0x1A2,
       OSC_GQR = 0x1A4, OSC_PSF = 0x1C8 };

static void switch_to(guest_thread *t) {
    guest_thread *from = cur;
    cur = t;
    plat_fiber_switch(from->fiber, t->fiber);
    /* resumed: cur was set by whoever switched back to us */
}

static void thread_main(void *arg) {
    guest_thread *t = arg;
    LOG("thread", "start %p entry=%08X sp=%08X", (void *)t, t->entry, t->ctx.r[1]);
    ppc_call_indirect(&t->ctx, g_base, t->entry);
    /* entry returned: continue at the context's LR (OSExitThread) with r3 = result */
    ppc_call_indirect(&t->ctx, g_base, t->ctx.lr);
    rt_fatal("guest thread %p returned from its exit routine", (void *)t);
}

void hle_OSSaveContext(ppc_context *ctx, uint8_t *base) {
    uint32_t c = ctx->r[3];
    for (int i = 0; i < 32; i++) st32(base, c + OSC_GPR + 4 * i, ctx->r[i]);
    st32(base, c + OSC_CR, ctx->cr);
    st32(base, c + OSC_LR, ctx->lr);
    st32(base, c + OSC_CTR, ctx->ctr);
    st32(base, c + OSC_XER, (ctx->xer_so << 31) | (ctx->xer_ov << 30) | (ctx->xer_ca << 29) | ctx->xer_bc);
    st32(base, c + OSC_SRR0, ctx->lr);
    st32(base, c + OSC_SRR1, ctx->msr);
    for (int i = 1; i < 8; i++) st32(base, c + OSC_GQR + 4 * i, ctx->gqr[i]);
    ctx_bind(c, ctx->lr, cur);
    ctx->r[3] = 0;
}

void hle_OSLoadContext(ppc_context *ctx, uint8_t *base) {
    uint32_t c = ctx->r[3];
    /* end of an interrupt handler: return to the poll point that took it (rfi) */
    if (cur->npoll && cur->polls[cur->npoll - 1].exc_ctx == c)
        RT_LONGJMP(cur->polls[cur->npoll - 1].jb);
    uint32_t srr0 = ld32(base, c + OSC_SRR0), bsrr0;
    guest_thread *t = ctx_bound(c, &bsrr0);
    if (t && bsrr0 == srr0) {
        if (t != cur) switch_to(t);
        return;
    }
    /* a context that was never saved: a new thread starting at srr0 */
    t = calloc(1, sizeof *t);
    for (int i = 0; i < 32; i++) t->ctx.r[i] = ld32(base, c + OSC_GPR + 4 * i);
    t->ctx.cr = ld32(base, c + OSC_CR);
    t->ctx.lr = ld32(base, c + OSC_LR);
    t->ctx.ctr = ld32(base, c + OSC_CTR);
    for (int i = 1; i < 8; i++) t->ctx.gqr[i] = ld32(base, c + OSC_GQR + 4 * i);
    t->ctx.msr = ld32(base, c + OSC_SRR1);
    t->ctx.fpscr = ld32(base, c + OSC_FPSCR);
    memcpy(t->ctx.spr, ctx->spr, sizeof ctx->spr);
    t->entry = srr0;
    t->fiber = plat_fiber_create(thread_main, t);
    ctx_bind(c, srr0, t);
    LOG("thread", "new thread for context %08X entry %08X (%s)", c, srr0, rt_func_name(srr0, NULL));
    switch_to(t);
}

/* ------------------------------------------------------------------ exceptions */
enum { EXC_EXTERNAL = 4, EXC_DECREMENTER = 8 };

static void take_exception(ppc_context *ctx, uint8_t *base, int n) {
    uint32_t handler = ld32(base, 0x80003000u + 4u * n);
    if (!handler) return;
    if (cur->npoll == MAX_POLL) return;
    uint32_t exc_ctx = ld32(base, 0x800000D4u);
    poll_frame *pf = &cur->polls[cur->npoll++];
    pf->exc_ctx = exc_ctx;
    pf->ctx = ctx;
    pf->snap = *ctx;
    st32(base, exc_ctx + OSC_SRR0, 0);
    st32(base, exc_ctx + OSC_SRR1, ctx->msr);
    if (RT_SETJMP(pf->jb) == 0) {
        ctx->spr[26] = 0; ctx->spr[27] = ctx->msr;      /* SRR0/SRR1 */
        ctx->msr &= ~0x8000u;                            /* EE off inside handlers */
        ctx->r[3] = (uint32_t)n; ctx->r[4] = exc_ctx;
        ppc_call_indirect(ctx, g_base, handler);
    }
    /* back from the handler (rfi). Only globals are trusted here: after __builtin_longjmp
       the compiler's register copies of locals may be stale. */
    {
        guest_thread *volatile *pc = (guest_thread *volatile *)&cur;
        guest_thread *t = *pc;
        poll_frame *f = &t->polls[t->npoll - 1];
        *f->ctx = f->snap;
        t->npoll--;
    }
}

void ppc_poll(ppc_context *ctx, uint8_t *base) {
    hw_update();
    if (!(ctx->msr & 0x8000u)) return;
    if (pi_active()) { take_exception(ctx, base, EXC_EXTERNAL); return; }
    if (dec_armed && (int64_t)(rt_tb_now() - dec_deadline) >= 0) {
        dec_armed = 0;
        take_exception(ctx, base, EXC_DECREMENTER);
    }
}

void ppc_rfi(ppc_context *ctx, uint8_t *base, uint32_t pc) {
    (void)base;
    /* only reached if some rfi path other than OSLoadContext runs */
    if (cur->npoll) RT_LONGJMP(cur->polls[cur->npoll - 1].jb);
    /* the SDK's real-mode trick: mtsrr0 <function>; rfi  ...  mtsrr0 <return addr>; rfi.
       Jumping to a function start = call it; anything else = return to our caller. */
    uint32_t target = ctx->spr[26];
    if (target < 0x80000000u) target |= 0x80000000u;
    ctx->msr = ctx->spr[27];
    if (lookup(target)) { LOG("cpu", "rfi-jump %08X -> %08X", pc, target); ppc_call_indirect(ctx, base, target); }
}

/* interrupt enable points: deliver anything pending right away */
void hle_OSEnableInterrupts(ppc_context *ctx, uint8_t *base) {
    orig_OSEnableInterrupts(ctx, base);
    uint32_t r3 = ctx->r[3];
    ppc_poll(ctx, base);
    ctx->r[3] = r3;
}
void hle_OSRestoreInterrupts(ppc_context *ctx, uint8_t *base) {
    orig_OSRestoreInterrupts(ctx, base);
    if (ctx->msr & 0x8000u) { uint32_t r3 = ctx->r[3]; ppc_poll(ctx, base); ctx->r[3] = r3; }
}

/* ------------------------------------------------------------------ OS conveniences */
static void guest_printf(ppc_context *ctx, uint8_t *base, int first_arg, char *out, size_t n);
void hle_OSReport(ppc_context *ctx, uint8_t *base) {
    char buf[1024]; guest_printf(ctx, base, 3, buf, sizeof buf);
    fprintf(stderr, "[OSReport] %s", buf);
    size_t l = strlen(buf); if (!l || buf[l - 1] != '\n') fputc('\n', stderr);
}
void hle_OSPanic(ppc_context *ctx, uint8_t *base) {
    char buf[1024]; guest_printf(ctx, base, 5, buf, sizeof buf);
    rt_fatal("OSPanic in %s line %u: %s", (char *)G(ctx->r[3]), ctx->r[4], buf);
}
void hle_PPCHalt(ppc_context *ctx, uint8_t *base) { (void)base; rt_fatal("PPCHalt called (lr=%08X)", ctx->lr); }

/* minimal printf over guest varargs: integer args from r(first)..r10, doubles from f1.. */
static void guest_printf(ppc_context *ctx, uint8_t *base, int first, char *out, size_t n) {
    const char *f = (const char *)(g_base + PPC_PHYS(ctx->r[first]));
    int gi = first + 1, fi = 1; size_t o = 0;
    (void)base;
    for (; *f && o + 64 < n; f++) {
        if (*f != '%') { out[o++] = *f; continue; }
        char spec[32]; int k = 0; spec[k++] = *f++;
        while (*f && strchr("-+ #0123456789.lhz", *f) && k < 28) spec[k++] = *f++;
        if (!*f) break;
        char c = *f; spec[k++] = c; spec[k] = 0;
        uint32_t iv = gi <= 10 ? ctx->r[gi] : 0;
        if (c == 's') { gi++; o += snprintf(out + o, n - o, spec, iv ? (char *)G(iv) : "(null)"); }
        else if (strchr("fFgGeE", c)) { o += snprintf(out + o, n - o, spec, fi <= 8 ? ctx->f[fi].d : 0.0); fi++; }
        else if (c == '%') out[o++] = '%';
        else if (strchr("diuxXoc", c)) {
            char sp2[32]; int j = 0;
            for (int i = 0; spec[i]; i++) if (spec[i] != 'l' && spec[i] != 'h' && spec[i] != 'z') sp2[j++] = spec[i];
            sp2[j] = 0; gi++;
            o += snprintf(out + o, n - o, sp2, (c == 'd' || c == 'i') ? (int)(int32_t)iv : (unsigned)iv);
        } else if (c == 'p') { gi++; o += snprintf(out + o, n - o, "%08X", iv); }
    }
    out[o] = 0;
}

/* REL modules were recompiled for fixed addresses; make sure the game put them there */
static const struct { const char *name; uint32_t text; } expected_modules[] = {
    {"d_profileNP", 0x807685E0}, {"d_basesNP", 0x8076D7B0}, {"d_enemiesNP", 0x809A2DD0}, {"d_en_bossNP", 0x80B1CA50},
};
static void check_module(ppc_context *ctx, uint8_t *base, void (*orig)(ppc_context *, uint8_t *)) {
    uint32_t module = ctx->r[3];
    uint32_t id = ld32(base, module);
    orig(ctx, base);
    uint32_t sect = ld32(base, module + 0x10);              /* absolute after linking */
    uint32_t text = ld32(base, sect + 8) & ~1u;
    if (id >= 1 && id <= 4) {
        if (text != expected_modules[id - 1].text)
            rt_fatal("module %s linked at %08X but was recompiled for %08X", expected_modules[id - 1].name, text, expected_modules[id - 1].text);
        rt_log("os", "module %s linked at %08X (matches recompiled code)", expected_modules[id - 1].name, text);
    } else rt_log("os", "OSLink: unknown module id %u at %08X", id, text);
}
void hle_OSLinkFixed(ppc_context *ctx, uint8_t *base) { check_module(ctx, base, orig_OSLinkFixed); }
void hle_OSLink(ppc_context *ctx, uint8_t *base) { check_module(ctx, base, orig_OSLink); }

/* ------------------------------------------------------------------ boot */
static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *d = malloc(n ? n : 1); if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f); if (len) *len = n; return d;
}

static void load_dol(void) {
    char p[1100]; snprintf(p, sizeof p, "%s/orig/main.dol", g_root);
    size_t n; uint8_t *d = read_file(p, &n); if (!d) rt_fatal("cannot read %s", p);
    for (int i = 0; i < 18; i++) {
        uint32_t off = BSWAP32(((uint32_t *)d)[i]), addr = BSWAP32(((uint32_t *)d)[18 + i]), sz = BSWAP32(((uint32_t *)d)[36 + i]);
        if (sz) memcpy(G(addr), d + off, sz);
    }
    free(d);
}

static void setup_low_memory(void) {
    char p[1100];
    size_t n; uint8_t *boot, *bi2, *fst;
    snprintf(p, sizeof p, "%s/sys/boot.bin", g_disc); boot = read_file(p, &n); if (!boot) rt_fatal("cannot read %s", p);
    snprintf(p, sizeof p, "%s/sys/bi2.bin", g_disc); bi2 = read_file(p, &n); if (!bi2) rt_fatal("cannot read %s", p);
    const uint8_t *disc_fst(uint32_t *len);
    uint32_t fl; const uint8_t *df = disc_fst(&fl);
    size_t fst_len = fl; fst = malloc(fl); memcpy(fst, df, fl);
    memcpy(G(0x80000000), boot, 0x20);                     /* disc ID */
    memcpy(G(0x80003180), boot, 4);                        /* game ID check copy */
    /* values from Dolphin's Wii BS2 emulation + IOS53 memory setup */
    W32(0x80000020, 0x0D15EA5E); W32(0x80000024, 1); W32(0x80000028, 0x01800000);
    W32(0x8000002C, 0x00000003);                           /* RVL retail */
    W32(0x800000CC, 0);                                     /* NTSC */
    W32(0x800000E4, 0x8008F7B8); W32(0x800000F0, 0x01800000);
    W32(0x800000F8, 0x0E7BE2C0); W32(0x800000FC, 0x2B73A840);
    W32(0x800030D8, 0xFFFFFFFF); W16(0x800030E6, 0x8201); W16(0x800030E0, 0);
    W32(0x80003100, 0x01800000); W32(0x80003104, 0x01800000); W32(0x80003108, 0x81800000);
    W32(0x8000310C, 0x00000000); W32(0x80003110, 0x81800000);
    W32(0x80003118, 0x04000000); W32(0x8000311C, 0x04000000); W32(0x80003120, 0x93600000);
    W32(0x80003124, 0x90000800); W32(0x80003128, 0x935E0000);
    W32(0x80003130, 0x935E0000); W32(0x80003134, 0x93600000);
    W32(0x80003138, 0x00000011); W32(0x80003140, 0x0035161F); W32(0x80003144, 0x00030110);
    W32(0x80003148, 0x93600000); W32(0x8000314C, 0x93620000); W32(0x80003158, 0x0000FF01);
    W8(0x8000315C, 0x80); W8(0x8000315D, 0xAD); W16(0x8000315E, 0x0113);
    W32(0x80003184, 0x80000000); W32(0x80003194, 0); W32(0x80003198, 0x0F800000 >> 2);
    W32(0x80003188, 0x0035161F);                           /* IOS the apploader says the game needs */
    /* setting.txt (encrypted) is left at 0x3800 by the system menu */
    { char sp[1200]; size_t sl; snprintf(sp, sizeof sp, "%s/nand/title/00000001/00000002/data/setting.txt", g_root);
      uint8_t *st = read_file(sp, &sl); if (st) { memcpy(G(0x80003800), st, sl > 0x100 ? 0x100 : sl); free(st); }
      else rt_log("boot", "no setting.txt - run tools/make_nand.py"); }
    /* what the apploader leaves: bi2 and the FST at the top of MEM1, arena below them */
    uint32_t fst_max = BSWAP32(*(uint32_t *)(boot + 0x42C)) << 2;
    if (fst_max < fst_len) fst_max = (uint32_t)fst_len;
    uint32_t bi2_addr = 0x817FE000 - 0x2000;
    uint32_t fst_addr = (bi2_addr - fst_max) & ~0x1Fu;
    memcpy(G(bi2_addr), bi2, 0x2000);
    memcpy(G(fst_addr), fst, fst_len);
    W32(0x800000F4, bi2_addr);
    W32(0x80000038, fst_addr); W32(0x8000003C, fst_max);
    W32(0x80000034, fst_addr);                              /* arena hi (legacy) */
    W32(0x80003110, fst_addr);                              /* MEM1 arena end: keep the heap below the FST */
    W32(0x80000030, 0);
    for (uint32_t a = 0x80003000; a <= 0x80003038; a += 4) W32(a, 0);
    free(boot); free(bi2); free(fst);
    rt_log("boot", "FST at %08X (%u bytes), bi2 at %08X", fst_addr, fst_max, bi2_addr);
}

int main(int argc, char **argv) {
    if (argc >= 3) {
        snprintf(g_root, sizeof g_root, "%s", argv[1]);
        snprintf(g_disc, sizeof g_disc, "%s", argv[2]);
    } else {
        /* Layouts, checked in order:
           game folder:  <dir>/nsmbw.exe, <dir>/orig/main.dol, <dir>/nand, disc in <dir>/disc or <dir>/../extracted
           dev build:    <project>/recomp/out/nsmbw.exe with data in <project>/recomp, disc in <project>/extracted */
        char exe[1024]; snprintf(exe, sizeof exe, "%s", argv[0]);
#ifdef _WIN32
        GetModuleFileNameA(NULL, exe, sizeof exe);           /* full path even when started from elsewhere */
#endif
        char *sl = strrchr(exe, '/'), *bs = strrchr(exe, '\\'); if (bs > sl) sl = bs;
        if (sl) *sl = 0; else snprintf(exe, sizeof exe, ".");
        char probe[1200]; FILE *pf;
        snprintf(probe, sizeof probe, "%s/orig/main.dol", exe);
        if ((pf = fopen(probe, "rb"))) { fclose(pf); snprintf(g_root, sizeof g_root, "%s", exe); }
        else snprintf(g_root, sizeof g_root, "%s/..", exe);
        snprintf(probe, sizeof probe, "%s/disc/sys/main.dol", g_root);
        if ((pf = fopen(probe, "rb"))) { fclose(pf); snprintf(g_disc, sizeof g_disc, "%s/disc", g_root); }
        else snprintf(g_disc, sizeof g_disc, "%s/../extracted", g_root);
    }
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "-v")) g_verbose = 1;
#ifdef _WIN32
    g_base = VirtualAlloc(NULL, 0x40000000ull, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_base) rt_fatal("cannot allocate guest memory");
#else
    g_base = mmap(NULL, 0x40000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_base == MAP_FAILED) rt_fatal("cannot allocate guest memory");
#endif
    t0_ns = rt_now_ns();
    plat_init();
    load_dol();
    void mod_load(void); mod_load();
    if (disc_init()) rt_fatal("cannot open disc files in %s", g_disc);
    setup_low_memory();
    hw_init(); ios_init(); gx_init();
    main_thread.fiber = plat_fiber_main();
    cur = &main_thread;
    ppc_context *ctx = &main_thread.ctx;
    ctx->msr = 0x00002032;                 /* FP, IR, DR, RI */
    ctx->r[1] = 0x816FFFF0;
    rt_log("boot", "jumping to __start");
    ppc_call_indirect(ctx, g_base, 0x80004050);
    rt_fatal("__start returned");
    return 0;
}

/* Debug: list live state-machine states. Finds "...::StateID_..." strings in guest memory, the
 * sStateID objects whose name field points at them, then words that point at those objects. */
static int cmpu32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static int has_u32(const uint32_t *v, int n, uint32_t x) { return bsearch(&x, v, n, 4, cmpu32) != NULL; }
void rt_state_scan(void) {
    static uint32_t str_addr[16384], np_addr[16384]; int nstr = 0, nnp = 0;
    const uint32_t regions[][2] = {{0x80000000u, 0x81800000u}, {0x90000000u, 0x94000000u}};
    for (int r = 0; r < 2; r++)
        for (uint32_t a = regions[r][0]; a + 12 < regions[r][1] && nstr < 16384; a++) {
            uint8_t *p = G(a);
            if (p[0] == ':' && p[1] == ':' && !memcmp(p + 2, "StateID_", 8)) {
                uint32_t b = a; while (b > regions[r][0] && G(b - 1)[0] >= 0x20 && G(b - 1)[0] < 0x7F) b--;
                str_addr[nstr++] = b; a += 10;
            }
        }
    qsort(str_addr, nstr, 4, cmpu32);
    for (int r = 0; r < 2; r++)
        for (uint32_t a = regions[r][0]; a + 4 <= regions[r][1] && nnp < 16384; a += 4)
            if (has_u32(str_addr, nstr, R32(a))) np_addr[nnp++] = a;    /* address of a name field */
    qsort(np_addr, nnp, 4, cmpu32);
    rt_log("state", "%d state names, %d state objects", nstr, nnp);
    const char *fu = getenv("RT_FINDU32");               /* list words equal to this value, with context */
    if (fu) {
        uint32_t val = (uint32_t)strtoul(fu, NULL, 16); int shown = 0;
        for (int r = 0; r < 2 && shown < 60; r++)
            for (uint32_t a = regions[r][0]; a + 4 <= regions[r][1] && shown < 60; a += 4) {
                if (R32(a) != val) continue;
                char buf[300]; int n = snprintf(buf, sizeof buf, "u32 at %08X:", a);
                for (int k = -8; k < 8 && n < 280; k++) n += snprintf(buf + n, sizeof buf - n, "%s%08X", k == 0 ? " [" : " ", R32(a + 4 * k));
                rt_log("find", "%s", buf); shown++;
            }
    }
    const char *dm = getenv("RT_DUMPMEM");               /* "addr,len" hex dump */
    if (dm) {
        uint32_t a = 0, len = 0; sscanf(dm, "%x,%x", &a, &len);
        for (uint32_t o = 0; o < len; o += 32) {
            char buf[200]; int n = snprintf(buf, sizeof buf, "%08X:", a + o);
            for (int k = 0; k < 8; k++) n += snprintf(buf + n, sizeof buf - n, " %08X", R32(a + o + 4 * k));
            rt_log("mem", "%s", buf);
        }
    }
    const char *want = getenv("RT_STATEOBJ");            /* dump the state objects whose name contains this */
    for (int i = 0; want && i < nnp; i++) {
        const char *nm = (const char *)G(R32(np_addr[i]));
        if (!strstr(nm, want)) continue;
        char buf[400]; int n = snprintf(buf, sizeof buf, "obj name@%08X %s:", np_addr[i], nm);
        for (int k = -2; k < 12 && n < 380; k++) n += snprintf(buf + n, sizeof buf - n, " %08X", R32(np_addr[i] + 4 * k));
        rt_log("state", "%s", buf);
    }
    for (int r = 0; r < 2; r++)
        for (uint32_t a = regions[r][0]; a + 4 <= regions[r][1]; a += 4) {
            uint32_t v = R32(a); if (v < 0x80000000u) continue;
            for (int off = 4; off <= 8; off += 4)
                if (has_u32(np_addr, nnp, v + off) && a != v && a + 4 != v + off) {
                    rt_log("state", "  %08X -> %s", a, (const char *)G(R32(v + off)));
                    break;
                }
        }
}
