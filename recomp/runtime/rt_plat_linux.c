/* rt_plat_linux.c - headless Linux platform: ucontext fibers, frames dumped as PPM files. */
#define _GNU_SOURCE
#include "rt.h"
#include <ucontext.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <sys/time.h>
#include <ucontext.h>

/* (guest thread fibers: rt_state.c) */

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

/* RT_HANGDUMP=sec: dump guest state when no frame was presented for that long */
extern uint64_t g_frames_presented;
#include <pthread.h>
#include <unistd.h>
static void *hang_thread(void *a) {
    int sec = (int)(intptr_t)a; uint64_t last = 0; int still = 0;
    for (;;) { sleep(1); uint64_t f = g_frames_presented; if (f == last && f > 0) { if (++still >= sec) on_alarm(0); } else still = 0; last = f; }
    return 0;
}
void plat_init(void) {
    const char *hd = getenv("RT_HANGDUMP");
    if (hd) { pthread_t t; pthread_create(&t, NULL, hang_thread, (void *)(intptr_t)atoi(hd)); }
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
    { const char *from = getenv("RT_DUMP_FROM"); if (from && frame_no < atoi(from)) return; }   /* first frame to dump */
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
void plat_idle_wait(uint64_t ns) { struct timespec t = {0, (long)ns}; nanosleep(&t, NULL); }
/* RT_AUTOPRESS="sec:mask[:ms],..." presses buttons at given wall-clock seconds after start (testing) */
uint32_t plat_buttons(void) {
    static uint64_t t0; static const char *spec; static int init;
    { extern void chars_test_frame(void); chars_test_frame(); }   /* rt_chars.c: RT_SELECT_SCRIPT */
    extern uint32_t g_retro_test_buttons;                 /* rt_retro.c: RT_RETRO_SCRIPT */
    if (!init) { init = 1; t0 = rt_now_ns(); spec = getenv("RT_AUTOPRESS"); }
    if (!spec) return g_retro_test_buttons;
    double t = (double)(rt_now_ns() - t0) / 1e9;
    { extern uint64_t g_frames_presented; static int fr = -1; if (fr < 0) fr = getenv("RT_AUTOPRESS_FRAMES") != NULL;
      if (fr) t = (double)g_frames_presented / 60.0; }   /* RT_AUTOPRESS_FRAMES=1: seconds of game frames (renderer speed doesn't matter) */
    uint32_t b = 0; const char *p = spec;
    while (*p) {
        double at = strtod(p, (char **)&p); if (*p != ':') break; p++;
        uint32_t m = (uint32_t)strtoul(p, (char **)&p, 16); double dur = 0.2;
        if (*p == ':') { p++; dur = strtod(p, (char **)&p) / 1000.0; }
        if (t >= at && t < at + dur) b |= m;
        if (*p == ',') p++; else break;
    }
    return b | g_retro_test_buttons;
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

/* real Wii Remotes: none on Linux - but for tests, RT_FAKE_WM="n" connects n pretend remotes
   (addresses AABBCC00000i) that answer like real ones (status, memory reads, acks, reporting
   modes), with buttons from RT_FAKE_WM_SCRIPT="remote:frame:buttons hex:frames,..." (remote
   0-7; buttons in report order: 0x0100 2, 0x0200 1, 0x0800 A, 0x0010 +, 0x0008 up...) and
   RT_FAKE_WM_GONE="remote:frame,..." taking one away. Their LED reports are logged. */
#define FWM 8
static struct { int on; uint8_t mode, cont, leds; uint8_t q[64][23]; int qn[64], nq; uint64_t last_frame; uint16_t last_btn; } fwm[FWM];
static int fwm_n = -1;
static void fwm_push(int i, const uint8_t *r, int n) { if (fwm[i].nq < 64) { memcpy(fwm[i].q[fwm[i].nq], r, n); fwm[i].qn[fwm[i].nq++] = n; } }
static uint16_t fwm_buttons(int i) {
    const char *sp = getenv("RT_FAKE_WM_SCRIPT"); if (!sp) return 0;
    uint16_t b = 0; const char *p = sp;
    while (*p) {
        long who = strtol(p, (char **)&p, 10); if (*p != ':') break; p++;
        long at = strtol(p, (char **)&p, 10); if (*p != ':') break; p++;
        uint32_t m = (uint32_t)strtoul(p, (char **)&p, 16); long dur = 10;
        if (*p == ':') { p++; dur = strtol(p, (char **)&p, 10); }
        if (who == i && (long)g_frames_presented >= at && (long)g_frames_presented < at + dur) b |= (uint16_t)m;
        if (*p == ',') p++; else break;
    }
    return b;
}
static void fwm_init(void) {
    if (fwm_n >= 0) return;
    const char *e = getenv("RT_FAKE_WM"); fwm_n = e ? atoi(e) : 0; if (fwm_n > FWM) fwm_n = FWM;
    for (int i = 0; i < fwm_n; i++) { fwm[i].on = 1; fwm[i].mode = 0x30; }
}
int plat_wm_list(plat_wm_dev *out, int max) {
    fwm_init();
    const char *gone = getenv("RT_FAKE_WM_GONE");
    for (const char *p = gone; p && *p; ) {
        long who = strtol(p, (char **)&p, 10); if (*p != ':') break; p++;
        long at = strtol(p, (char **)&p, 10);
        if (who >= 0 && who < fwm_n && (long)g_frames_presented >= at && fwm[who].on) { fwm[who].on = 0; rt_log("fakewm", "remote %ld goes away", who); }
        if (*p == ',') p++; else break;
    }
    int n = 0;
    for (int i = 0; i < fwm_n && n < max; i++) if (fwm[i].on) { out[n].id = 100 + i; out[n].addr = 0xAABBCC000000ull + (uint64_t)i; n++; }
    return n;
}
void plat_wm_write(int id, const uint8_t *r, int len) {
    int i = id - 100; if (i < 0 || i >= fwm_n || !fwm[i].on || len < 2) return;
    uint16_t b = fwm_buttons(i); uint8_t b1 = b & 0x1F, b2 = (b >> 8) & 0x9F;
    int ack = r[1] & 0x02;
    switch (r[0]) {
    case 0x11: if (fwm[i].leds != (r[1] >> 4)) rt_log("fakewm", "remote %d lights %c%c%c%c", i, r[1] & 0x10 ? '1' : '0', r[1] & 0x20 ? '1' : '0', r[1] & 0x40 ? '1' : '0', r[1] & 0x80 ? '1' : '0');
        fwm[i].leds = r[1] >> 4; break;
    case 0x12: fwm[i].cont = (r[1] & 4) != 0; fwm[i].mode = len > 2 ? r[2] : 0x30; break;
    case 0x15: { uint8_t q[7] = {0x20, b1, b2, (uint8_t)(fwm[i].leds << 4), 0, 0, 0xC0}; fwm_push(i, q, 7); return; }
    case 0x16: { uint8_t q[5] = {0x22, b1, b2, 0x16, 0}; fwm_push(i, q, 5); return; }
    case 0x17: {
        int left = (r[5] << 8) | r[6]; uint32_t addr = (r[2] << 16) | (r[3] << 8) | r[4];
        int reg = (r[1] & 4) != 0, err = reg && ((addr >> 16) & 0xFE) == 0xA4 ? 7 : 0;
        while (left > 0) { int k = left > 16 ? 16 : left; uint8_t q[22] = {0x21, b1, b2, (uint8_t)(((k - 1) << 4) | err), (uint8_t)(addr >> 8), (uint8_t)addr};
            if (!reg && addr < 0x30) { static const uint8_t cal[0x30] = {0x66,0x6E,0x77,0xB8,0x83,0x71,0x89,0x88,0x30,0,0,0x66,0x6E,0x77,0xB8,0x83,0x71,0x89,0x88,0x30,0,0,0x80,0x80,0x80,0,0x9A,0x9A,0x9A,0,0x40,0xE3,0x80,0x80,0x80,0,0x9A,0x9A,0x9A,0,0x40,0xE3};
                for (int j = 0; j < k && addr + j < 0x30; j++) q[6 + j] = cal[addr + j]; }
            fwm_push(i, q, 22); if (err) break; addr += k; left -= k; }
        return;
    }
    }
    if (ack) { uint8_t q[5] = {0x22, b1, b2, r[0], 0}; fwm_push(i, q, 5); }
    if (r[0] == 0x12) fwm[i].last_btn = 0xFFFF;                 /* report in the new mode */
}
int plat_wm_read(int id, uint8_t *r, int max) {
    int i = id - 100; if (i < 0 || i >= fwm_n || !fwm[i].on) return 0;
    if (!fwm[i].nq) {                                           /* a data report when buttons change / continuous */
        uint16_t b = fwm_buttons(i);
        if (b != fwm[i].last_btn || (fwm[i].cont && g_frames_presented != fwm[i].last_frame)) {
            uint8_t q[22]; memset(q, 0, sizeof q); q[0] = fwm[i].mode; q[1] = b & 0x1F; q[2] = (b >> 8) & 0x9F;
            int n = fwm[i].mode == 0x30 ? 3 : fwm[i].mode == 0x31 ? 6 : fwm[i].mode == 0x33 ? 18 : 22;
            if (n > 3) { q[3] = 0x80; q[4] = 0x80; q[5] = 0x9A; }
            if (n > 6) memset(q + 6, 0xFF, n - 6);
            fwm_push(i, q, n); fwm[i].last_btn = b; fwm[i].last_frame = g_frames_presented;
        }
    }
    if (!fwm[i].nq) return 0;
    int n = fwm[i].qn[0] < max ? fwm[i].qn[0] : max; memcpy(r, fwm[i].q[0], n);
    memmove(fwm[i].q, fwm[i].q + 1, sizeof fwm[i].q[0] * (fwm[i].nq - 1)); memmove(fwm[i].qn, fwm[i].qn + 1, sizeof fwm[i].qn[0] * (fwm[i].nq - 1)); fwm[i].nq--;
    return n;
}
int plat_wm_pair_supported(void) { return 0; }
void plat_wm_pair(int on) { (void)on; }
int plat_wm_pair_state(char *msg, int n) { if (msg && n) snprintf(msg, n, "Pairing isn't available here"); return -1; }
int plat_wm_forget(uint64_t addr) { (void)addr; return 0; }
/* RT_FAKE_WM_PAIRED="frame:remote,...": pretend that fake remote was just paired then */
uint64_t plat_wm_pair_last(unsigned *gen) {
    static const char *spec; static int init; static unsigned g; static uint64_t last;
    if (!init) { init = 1; spec = getenv("RT_FAKE_WM_PAIRED"); }
    for (const char *p = spec; p && *p; ) {
        long at = strtol(p, (char **)&p, 10); if (*p != ':') break; p++;
        long r = strtol(p, (char **)&p, 10);
        if ((long)g_frames_presented >= at && (unsigned)(r + 1) > g) { g = (unsigned)(r + 1); last = 0xAABBCC000000ull + (uint64_t)r; }
        if (*p == ',') p++; else break;
    }
    *gen = g; return last;
}
void plat_set_fullscreen(int on) { (void)on; }
int plat_get_fullscreen(void) { return 0; }
void plat_set_window(int w, int h, int fixed) { (void)w; (void)h; (void)fixed; }
void plat_title_note(const char *note) { static char last[120]; if (note && strcmp(last, note)) { snprintf(last, sizeof last, "%s", note); rt_log("title", "%s", note); } else if (!note) last[0] = 0; }
void plat_restart(void) { rt_log("linux", "restart requested"); exit(0); }

/* typed characters (the Online screen's address): none here; tests use RT_NET_JOIN */
/* RT_TYPE="sec:text" (testing): typed on the keyboard from that many seconds of game frames on; '|' = Enter */
int plat_text_char(void) {
    static const char *p; static double at = -1;
    if (at < 0) { const char *e = getenv("RT_TYPE"); at = 1e9; if (e && strchr(e, ':')) { at = atof(e); p = strchr(e, ':') + 1; } }
    extern uint64_t g_frames_presented;
    if (!p || !*p || (double)g_frames_presented / 60.0 < at) return 0;
    char c = *p++; return c == '|' ? 13 : (unsigned char)c;
}
