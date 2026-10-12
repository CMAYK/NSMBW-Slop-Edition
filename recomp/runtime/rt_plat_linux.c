/* rt_plat_linux.c - headless Linux platform: ucontext fibers, frames dumped as PPM files. */
#define _GNU_SOURCE
#include "rt.h"
#include <ucontext.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <sys/time.h>
#include <ucontext.h>

struct rt_fiber { ucontext_t uc; void (*fn)(void *); void *arg; };
static rt_fiber main_fiber;

rt_fiber *plat_fiber_main(void) { return &main_fiber; }
static void trampoline(unsigned hi, unsigned lo) {
    rt_fiber *f = (rt_fiber *)(((uintptr_t)hi << 32) | lo);
    f->fn(f->arg);
}
rt_fiber *plat_fiber_create(void (*fn)(void *), void *arg) {
    rt_fiber *f = calloc(1, sizeof *f);
    size_t sz = 16u << 20;
    void *stack = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = stack; f->uc.uc_stack.ss_size = sz; f->uc.uc_link = NULL;
    f->fn = fn; f->arg = arg;
    uintptr_t p = (uintptr_t)f;
    makecontext(&f->uc, (void (*)(void))trampoline, 2, (unsigned)(p >> 32), (unsigned)p);
    return f;
}
void plat_fiber_switch(rt_fiber *from, rt_fiber *to) { swapcontext(&from->uc, &to->uc); }

extern uint64_t g_vi_fields, g_gx_draws, g_gx_cmds;
static uint64_t rt_prof_buf[1 << 20]; static volatile int prof_n;
static void on_prof(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    if (prof_n < (1 << 20)) rt_prof_buf[prof_n++] = (uint64_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
}
static void prof_dump(void) {
    char p[1200]; snprintf(p, sizeof p, "%s/profile_samples.txt", g_root);
    FILE *f = fopen(p, "w"); if (!f) return;
    for (int i = 0; i < prof_n; i++) fprintf(f, "%llx\n", (unsigned long long)rt_prof_buf[i]);
    fclose(f);
}
static void on_alarm(int sig) {
    (void)sig;
    prof_dump();
    void *bt[64]; int n = backtrace(bt, 64);
    fprintf(stderr, "host backtrace (addr2line -f -e out/nsmbw):");
    for (int i = 0; i < n; i++) fprintf(stderr, " %p", bt[i]);
    fprintf(stderr, "\n");
    fprintf(stderr, "\n*** watchdog: %llu VI fields, %llu GX cmds, %llu draws\n",
            (unsigned long long)g_vi_fields, (unsigned long long)g_gx_cmds, (unsigned long long)g_gx_draws);
    rt_dump_trace();
    _exit(2);
}
static void on_segv(int sig) {
    fprintf(stderr, "\n*** host signal %d\n", sig);
    rt_dump_trace();
    _exit(3);
}

/* NSMBW_PROFILE=1: SIGPROF sampler (1 kHz of CPU time); leaf addresses -> nsmbw_profile.txt on exit */
#define LPROF_N 65536
static uintptr_t lprof_pc[LPROF_N]; static uint32_t lprof_cnt[LPROF_N]; static uint64_t lprof_samples;
static void prof_sig(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    uintptr_t pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
    uint32_t h = (uint32_t)(pc * 2654435761u) & (LPROF_N - 1);
    for (int k = 0; k < 64; k++) { uint32_t i = (h + k) & (LPROF_N - 1);
        if (lprof_pc[i] == pc) { lprof_cnt[i]++; break; }
        if (!lprof_pc[i]) { lprof_pc[i] = pc; lprof_cnt[i] = 1; break; } }
    lprof_samples++;
}
static void prof_write(void) {
    char p[1200]; snprintf(p, sizeof p, "%s/nsmbw_profile.txt", g_root);
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "# samples %llu\n", (unsigned long long)lprof_samples);
    for (int i = 0; i < LPROF_N; i++) if (lprof_cnt[i] >= 2) fprintf(f, "%u %lX 0 0\n", lprof_cnt[i], (unsigned long)lprof_pc[i]);
    fclose(f);
}
static void prof_term(int sig) { (void)sig; prof_write(); _exit(0); }
static void prof_start(void) {
    const char *e = getenv("NSMBW_PROFILE"); if (!e || *e != '1') return;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = prof_sig; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    signal(SIGTERM, prof_term); signal(SIGINT, prof_term);
    struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, NULL);
}

void plat_init(void) {
    const char *w = getenv("RT_WATCHDOG");
    if (w) { signal(SIGALRM, on_alarm); alarm(atoi(w)); }
    if (getenv("RT_PROFILE")) {
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, NULL);
        struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, NULL);
    }
    signal(SIGSEGV, on_segv); signal(SIGBUS, on_segv); signal(SIGFPE, on_segv);
    prof_start();
}
static int frame_no;
void plat_present(const uint32_t *rgba, int w, int h) {
    frame_no++;
    const char *every = getenv("RT_DUMP_EVERY");
    int n = every ? atoi(every) : 60;
    if (n <= 0 || frame_no % n) return;
    const char *dd = getenv("RT_DUMP_DIR"); char p[1200];
    if (dd) snprintf(p, sizeof p, "%s/frame_%05d.ppm", dd, frame_no); else snprintf(p, sizeof p, "%s/frames/frame_%05d.ppm", g_root, frame_no);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint32_t c = rgba[i]; fputc((c >> 16) & 0xFF, f); fputc((c >> 8) & 0xFF, f); fputc(c & 0xFF, f); }
    fclose(f);
}
int plat_pump(void) { return 1; }
int plat_gl_present_begin(int *x, int *y, int *w, int *h, int *cw, int *ch) { (void)x; (void)y; (void)w; (void)h; (void)cw; (void)ch; return 0; }
void plat_gl_present_end(void) {}
/* RT_AUTOPRESS="sec:mask[:ms],..." presses buttons at given wall-clock seconds after start (testing) */
uint32_t plat_buttons(void) {
    static uint64_t t0; static const char *spec; static int init;
    if (!init) { init = 1; t0 = rt_now_ns(); spec = getenv("RT_AUTOPRESS"); }
    if (!spec) return 0;
    double t = (double)(rt_now_ns() - t0) / 1e9;
    uint32_t b = 0; const char *p = spec;
    while (*p) {
        double at = strtod(p, (char **)&p); if (*p != ':') break; p++;
        uint32_t m = (uint32_t)strtoul(p, (char **)&p, 16); double dur = 0.2;
        if (*p == ':') { p++; dur = strtod(p, (char **)&p) / 1000.0; }
        if (t >= at && t < at + dur) b |= m;
        if (*p == ',') p++; else break;
    }
    return b;
}

/* ------------------------------------------------------------------ OpenGL context (EGL, headless)
 * Surfaceless EGL context for testing the GL renderer without a display (Mesa llvmpipe works). */
static void *(*p_eglGetProcAddress)(const char *);
void *plat_gl_proc(const char *name) { return p_eglGetProcAddress ? p_eglGetProcAddress(name) : NULL; }
int plat_gl_create(void) {
    void *h = dlopen("libEGL.so.1", RTLD_NOW);
    if (!h) { rt_log("gl", "libEGL.so.1 not found"); return 0; }
    p_eglGetProcAddress = (void *(*)(const char *))dlsym(h, "eglGetProcAddress");
    void *(*getplat)(unsigned, void *, const int *) = (void *(*)(unsigned, void *, const int *))p_eglGetProcAddress("eglGetPlatformDisplayEXT");
    unsigned (*init)(void *, int *, int *) = (unsigned (*)(void *, int *, int *))dlsym(h, "eglInitialize");
    unsigned (*bind)(unsigned) = (unsigned (*)(unsigned))dlsym(h, "eglBindAPI");
    unsigned (*choose)(void *, const int *, void **, int, int *) = (unsigned (*)(void *, const int *, void **, int, int *))dlsym(h, "eglChooseConfig");
    void *(*create)(void *, void *, void *, const int *) = (void *(*)(void *, void *, void *, const int *))dlsym(h, "eglCreateContext");
    unsigned (*make)(void *, void *, void *, void *) = (unsigned (*)(void *, void *, void *, void *))dlsym(h, "eglMakeCurrent");
    if (!getplat || !init || !bind || !choose || !create || !make) return 0;
    void *d = getplat(0x31DD, NULL, NULL);                   /* EGL_PLATFORM_SURFACELESS_MESA */
    int ma, mi; if (!d || !init(d, &ma, &mi)) return 0;
    bind(0x30A2);                                            /* EGL_OPENGL_API */
    const int ca[] = {0x3040, 0x0008, 0x3033, 0, 0x3038};    /* RENDERABLE_TYPE=OPENGL_BIT, SURFACE_TYPE=0 */
    void *cfg = NULL; int n = 0; choose(d, ca, &cfg, 1, &n);
    const int attr[] = {0x3098, 3, 0x30FB, 3, 0x30FD, 1, 0x3038};
    void *c = create(d, n ? cfg : NULL, NULL, attr);
    if (!c || !make(d, NULL, NULL, c)) return 0;
    return 1;
}

/* audio: RT_AUDIO_WAV=path writes the output to a WAV file (for testing) */
void plat_audio_push(const int16_t *lr, int frames) {
    static FILE *f; static int init; static uint32_t total;
    if (!init) { init = 1; const char *p = getenv("RT_AUDIO_WAV"); if (p && (f = fopen(p, "wb"))) fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x00\x7d\0\0\0\xf4\x01\0\x04\0\x10\0data\0\0\0\0", 1, 44, f); }
    if (!f) return;
    fwrite(lr, 4, (size_t)frames, f); total += (uint32_t)frames * 4;
    long here = ftell(f); uint32_t riff = total + 36;
    fseek(f, 4, SEEK_SET); fwrite(&riff, 4, 1, f); fseek(f, 40, SEEK_SET); fwrite(&total, 4, 1, f); fseek(f, here, SEEK_SET);
}

/* real Wii Remote passthrough: not implemented on Linux yet */
int plat_wiimote_present(void) { return 0; }
void plat_wiimote_write(const uint8_t *r, int len) { (void)r; (void)len; }
int plat_wiimote_read(uint8_t *r, int max) { (void)r; (void)max; return 0; }
void plat_set_fullscreen(int on) { (void)on; }
int plat_get_fullscreen(void) { return 0; }
void plat_set_window(int w, int h, int fixed) { (void)w; (void)h; (void)fixed; }
void plat_restart(void) { rt_log("linux", "restart requested"); exit(0); }
