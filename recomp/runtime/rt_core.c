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
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

uint8_t *g_base RT_LOCAL;
char g_root[1024] RT_LOCAL, g_disc[1024] RT_LOCAL;
int g_verbose RT_LOCAL = 0;
volatile uint32_t ppc_poll_counter;
#ifdef PPC_TRACE
uint32_t ppc_trace_ring[256] RT_LOCAL; uint32_t ppc_trace_pos RT_LOCAL;
/* RT_FUNCSTAT="first,count": histogram of recently entered guest functions, sampled once per VI
 * field over that range of fields, printed at the end (debug aid for "what is the game doing"). */
static uint32_t fs_key[1 << 16] RT_LOCAL, fs_cnt[1 << 16] RT_LOCAL;
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
        if (!((t >= 0x80000000u && t < 0x81800000u) || (t >= 0x90000000u && t < 0x94000000u))) { fprintf(stderr, "  (bad thread pointer %08X)\n", t); break; }
        fprintf(stderr, "  thread %08X state %u prio %d suspend %d\n", t, R16(t + 0x2C8), (int)R32(t + 0x2D0), (int)R32(t + 0x2CC));
        backtrace_guest(R32(t + 4), R32(t + 0x84));
    }
}
void rt_dump_trace(void) {
    rt_dump_threads();
    { extern void rt_hang_state(void); rt_hang_state(); }
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
    /* gameplay options (the 8-player features, read when the game starts) */
    {"shake_with_button", 0, "1 = a button does the shake/spin (no motion needed)"},
    {"death_messages", 0, "show messages when players die: 0 = off, 1 = on all damage, 2 = only on death"},
    {"infinite_lives", 0, "1 = count deaths instead of lives (no game over)"},
    {"no_powerup_pause", 0, "1 = don't pause the game when someone powers up"},
    {"always_exit_course", 0, "1 = Exit Course is always available in the pause menu"},
    {"hide_motion_guides", 1, "hide motion control speech bubbles: 0 = never, 1 = with 5+ players, 2 = always"},
    {"yoshi_colors", 0, "Yoshi colors: 0 = normal, 1 = random, 2 = always green"},
    {"unlock_all_worlds", 0, "1 = all worlds available"},
    {"all_paths_open", 0, "1 = all world map paths available"},
    {"liftable_tiles", 0, "1 = liftable tiles"},
    {"bubble_swarm", 0, "1 = 1-Up bubble swarm mode"},
    {"deadly_1up", 0, "1 = deadly 1-ups"},
    /* retro characters (rt_retro.c) */
    {"retro_mario", 0, "testing: 1 = every Mario plays as SMB1 Mario (normally picked in the character select)"},
    {"retro_sfx_volume", 70, "volume of the retro characters' sound effects, 0-100"},
    /* online play (rt_net.c) */
    {"net_port", 41790, "online: the UDP port you host on (forward it on your router)"},
    {"net_rollback", 1, "online (host): 1 = rollback (your own buttons act at once, the others' are guessed and corrected), 0 = lockstep"},
    {"net_input_delay", 0, "online (host): input delay in video fields (17 ms each). Rollback: 0 = none (default), 1-8; lockstep: 0 = automatic from the pings, 1-15"},
    /* updates (rt_update.c) */
    {"discord_presence", 1, "1 = show what you're playing on your Discord profile (when the Discord app is running), 0 = off"},
    {"update_check", 1, "1 = look for a new version of the game at start (and download it), 0 = never"},
};
/* keys the 8-player options had while they were a separate mod ("mod_..."): still read, and
   rewritten to the new names the first time settings.ini is loaded */
static const char *setting_aliases[][2] = {
    {"mod_unlocked_all_worlds", "unlock_all_worlds"}, {"mod_completed_all_levels", "complete_all_levels"},
    {"mod_no_powerup_pause", "no_powerup_pause"}, {"mod_infinite_lives", "infinite_lives"}, {"mod_death_messages", "death_messages"},
    {"mod_fall_damage", "fall_damage"}, {"mod_skip_opening_movie", "skip_opening_movie"}, {"mod_always_exit_course", "always_exit_course"},
    {"mod_shake_with_button", "shake_with_button"}, {"mod_hide_motion_guides", "hide_motion_guides"}, {"mod_yoshi_colors", "yoshi_colors"},
    {"mod_liftable_tiles", "liftable_tiles"}, {"mod_bubble_swarm", "bubble_swarm"}, {"mod_deadly_1up", "deadly_1up"},
    {"mod_all_paths_open", "all_paths_open"},
};
static const char *setting_new_name(const char *k) {
    for (size_t i = 0; i < sizeof setting_aliases / sizeof *setting_aliases; i++) if (!strcmp(k, setting_aliases[i][0])) return setting_aliases[i][1];
    return NULL;
}
static void settings_migrate_file(const char *p) {        /* "mod_x = v" -> "x = v", "; 8-player mod: help" -> new help */
    FILE *f = fopen(p, "r"); if (!f) return;
    char *out = NULL; size_t n = 0, cap = 0; int changed = 0; char line[512];
    while (fgets(line, sizeof line, f)) {
        char k[64]; int v; const char *nk;
        if (line[0] != ';' && sscanf(line, " %63[A-Za-z0-9_] = %d", k, &v) == 2 && (nk = setting_new_name(k))) { snprintf(line, sizeof line, "%s = %d\n", nk, v); changed = 1; }
        else if (!strncmp(line, "; 8-player mod: ", 16)) { memmove(line + 2, line + 16, strlen(line + 16) + 1); changed = 1; }
        else if (!strncmp(line, "; online (host): input delay in video fields, 1-8", 49)) {   /* (the old fixed default 3: now automatic) */
            snprintf(line, sizeof line, "; online (host): input delay in video fields (17 ms each). Rollback: 0 = none (default), 1-8; lockstep: 0 = automatic from the pings, 1-15\n"); changed = 2; }
        else if (changed == 2 && sscanf(line, " net_input_delay = %d", &v) == 1) { if (v == 3) snprintf(line, sizeof line, "net_input_delay = 0\n"); changed = 1; }
        size_t l = strlen(line);
        if (n + l + 1 > cap) { cap = (n + l + 1) * 2; out = realloc(out, cap); }
        memcpy(out + n, line, l + 1); n += l;
    }
    fclose(f);
    if (changed && (f = fopen(p, "w"))) { fwrite(out, 1, n, f); fclose(f); rt_log("settings", "renamed the old mod_* options in settings.ini"); }
    free(out);
}
static char setting_keys[64][64] RT_LOCAL; static int setting_vals[64] RT_LOCAL, nsettings RT_LOCAL, settings_loaded RT_LOCAL;
/* settings each PC keeps for itself in an online game (the rest are the host's: online/settings.ini) */
static int setting_is_local(const char *k) {
    static const char *local[] = {"render_scale", "window_width", "window_height", "fixed_window_size", "fullscreen",
                                  "texture_filter", "retro_sfx_volume", "net_port", "net_input_delay", "update_check", "discord_presence", NULL};
    for (int i = 0; local[i]; i++) if (!strcmp(k, local[i])) return 1;
    return 0;
}
static void settings_take_hosts(void) {
    int n = 0;
    for (int i = 0; i < nsettings; i++) if (setting_is_local(setting_keys[i])) { if (n != i) { memcpy(setting_keys[n], setting_keys[i], 64); setting_vals[n] = setting_vals[i]; } n++; }
    nsettings = n;
    char p[1300]; snprintf(p, sizeof p, "%s/settings.ini", rt_session_dir());
    FILE *f = fopen(p, "r"); char line[256];
    while (f && fgets(line, sizeof line, f) && nsettings < 64) {
        char k[64]; int v;
        if (line[0] == ';' || line[0] == '#') continue;
        if (sscanf(line, " %63[A-Za-z0-9_] = %d", k, &v) == 2 && !setting_is_local(k)) { snprintf(setting_keys[nsettings], 64, "%s", k); setting_vals[nsettings++] = v; }
    }
    if (f) fclose(f);
    rt_log("settings", "online game: the host's game settings");
}
void rt_settings_reload(void) { settings_loaded = 0; nsettings = 0; }   /* (an online game starts / ends) */
int rt_setting(const char *key, int def) {
    if (!settings_loaded) {
        settings_loaded = 1;
        char p[1200]; snprintf(p, sizeof p, "%s/settings.ini", g_root);
        settings_migrate_file(p);
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
        if (session_on()) settings_take_hosts();
    }
    for (int i = 0; i < nsettings; i++) if (!strcmp(setting_keys[i], key)) return setting_vals[i];
    { const char *nk = setting_new_name(key); if (nk) return rt_setting(nk, def); }   /* asked by an old name */
    return def;
}
/* change a setting and save it: the "key = value" line in settings.ini is rewritten in place */
void rt_setting_set(const char *key, int val) {
    rt_setting(key, 0);
    int found = 0;
    for (int i = 0; i < nsettings; i++) if (!strcmp(setting_keys[i], key)) { setting_vals[i] = val; found = 1; }
    if (!found && nsettings < 64) { snprintf(setting_keys[nsettings], 64, "%s", key); setting_vals[nsettings++] = val; }
    char p[1200]; snprintf(p, sizeof p, "%s/settings.ini", g_root);
    if (session_on() && !setting_is_local(key)) snprintf(p, sizeof p, "%s/settings.ini", rt_session_dir());   /* (the session's own copy) */
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
static uint64_t t0_ns RT_LOCAL; static int64_t tb_offset;
static uint64_t os_now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER f RT_LOCAL; LARGE_INTEGER c;
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
static double tsc_scale RT_LOCAL; static uint64_t tsc0 RT_LOCAL, tns0 RT_LOCAL; static volatile int clk_state RT_LOCAL;
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
/* ---- deterministic mode (online play, docs/ONLINE.md) ----
 * Normally the emulated hardware runs on the PC's clock. With g_det on, it runs on a virtual
 * clock instead: g_vt_ns advances by VT_POLL_NS at every poll point and by VT_TB_NS at every
 * time-base read (a spin loop on OSGetTime still ends), and jumps to the next timed event when
 * the guest is idle; the PC's clock only paces it (det_pace). The same code with the same
 * inputs then does the same thing on every PC, at the same virtual times. */
int g_det RT_LOCAL;                         /* (the session decides: not part of a state) */
uint64_t g_vt_ns = 1000000000ull;
#define VT_POLL_NS 2000ull
#define VT_TB_NS   20ull
uint64_t rt_emu_ns(void) { return g_det ? g_vt_ns : rt_now_ns(); }
/* switch to the virtual clock now (an online game starts), the clocks going on without a jump */
static uint64_t det_t0;
void rt_det_enable(void) {
    if (g_det) return;
    uint64_t now = rt_now_ns();
    g_vt_ns = now; det_t0 = t0_ns; g_det = 1;
    rt_log("cpu", "deterministic mode from now on (online game)");
}
static uint64_t det_t0;
uint64_t rt_tb_now(void) {
    /* (one multiply instead of two 64-bit divisions: this runs on every poll and every OSGetTick;
       a double holds ns exactly for ~100 days and the scaling is monotonic) */
    uint64_t ns = g_det ? g_vt_ns - det_t0 : rt_now_ns() - t0_ns;
    return (uint64_t)((int64_t)((double)ns * ((double)TB_HZ / 1e9)) + tb_offset);
}
uint64_t ppc_timebase(void) { if (g_det) g_vt_ns += VT_TB_NS; return rt_tb_now(); }
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
    {   static int lg = -1; if (lg < 0) lg = getenv("RT_DEC_LOG") != NULL;
        if (lg) { extern uint64_t g_vi_fields; rt_log("dec", "set field %llu vt %llu v %d lr %08X", (unsigned long long)g_vi_fields, (unsigned long long)g_vt_ns, (int32_t)v, ctx->lr); } }
}

/* ------------------------------------------------------------------ function lookup */
/* indirect calls (C++ virtual calls, callbacks - thousands per frame) look the target up in a
   small direct-mapped cache before the binary search over ~40,000 functions */
static struct { uint32_t addr; ppc_func fn; } icache[1 << 14] RT_LOCAL;
static ppc_func lookup_slow(uint32_t addr);
static inline ppc_func lookup(uint32_t addr) {
    uint32_t i = (addr >> 2) & ((1 << 14) - 1);
    if (icache[i].addr == addr && icache[i].fn) return icache[i].fn;
    ppc_func f = lookup_slow(addr);
    if (f) { icache[i].addr = addr; icache[i].fn = f; }
    return f;
}
/* native functions given a guest address (rt_register_native): for vtables and callbacks that
   guest code calls into native code through (e.g. a draw proc's drawOpa) */
static struct { uint32_t addr; ppc_func fn; } native_fns[32] RT_LOCAL;
static int native_count RT_LOCAL;
void rt_register_native(uint32_t addr, ppc_func fn) {
    for (int i = 0; i < native_count; i++) if (native_fns[i].addr == addr) { native_fns[i].fn = fn; return; }
    if (native_count < 32) { native_fns[native_count].addr = addr; native_fns[native_count].fn = fn; native_count++; }
}
static ppc_func lookup_slow(uint32_t addr) {
    uint32_t lo = 0, hi = ppc_func_count;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (ppc_func_table[m].addr == addr) return ppc_func_table[m].fn;
        if (ppc_func_table[m].addr < addr) lo = m + 1; else hi = m;
    }
    for (int i = 0; i < native_count; i++) if (native_fns[i].addr == addr) return native_fns[i].fn;
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
    uint32_t defer_sda;             /* != 0: preemption was deferred for this handler (see below) */
    uint64_t switches;              /* fiber switch count when the interrupt was taken (RT_PREEMPTLOG) */
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
#define MAX_THREADS 46
static guest_thread thread_pool[MAX_THREADS]; static int nthreads;

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

static uint64_t g_fiber_switches;
static void switch_to(guest_thread *t) {
    g_fiber_switches++;
    {   static long lf = -2, lt; if (lf == -2) { const char *e = getenv("RT_POLL_LOG"); lf = -1; if (e) sscanf(e, "%ld,%ld", &lf, &lt); }
        extern uint64_t g_vi_fields;
        if (lf >= 0 && (long)g_vi_fields >= lf && (long)g_vi_fields <= lt)
            rt_log("poll", "switch %d -> %d vt %llu cnt %u tlr %08X tr1 %08X", cur == &main_thread ? -1 : (int)(cur - thread_pool), t == &main_thread ? -1 : (int)(t - thread_pool), (unsigned long long)g_vt_ns, ppc_poll_counter, t->ctx.lr, t->ctx.r[1]); }
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
    if (nthreads == MAX_THREADS) rt_fatal("too many guest threads");
    t = &thread_pool[nthreads++];                  /* (a static pool: thread records are part of a save state) */
    memset(t, 0, sizeof *t);
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

/* ---- deferred preemption (fix for the "random hang": game frozen, audio still running)
 *
 * Interrupts are only delivered at poll points: loop back-edges (every 256th) and the
 * OSEnable/RestoreInterrupts calls. A back-edge is often exactly between a check and a sleep,
 * e.g. the mDvd command thread:   for (;;) { OSSleepThread(q); while ((c = first)) run(c); }
 * and mDvd_command_c::waitDone:    while (!done) OSSleepThread(q);
 * If an interrupt there wakes a higher-priority thread, the OS switches to it on the spot (the
 * interrupt handler ends with __OSReschedule). That thread queues the next disc command and
 * wakes the queue - but the command thread isn't on it yet: it was preempted right before going
 * to sleep, and then sleeps forever. The main thread then waits forever in waitDone. On a Wii
 * the window is a few instructions and an interrupt landing in it is practically impossible;
 * here every pending interrupt gets delivered at that one poll point, so it happens every few
 * minutes ("random" freezes, mostly while something loads).
 *
 * Fix: interrupt handlers still run at once (flags, callbacks, wake-ups all happen), but the
 * thread switch they ask for is postponed: the guest's scheduler lock (Reschedule, what
 * OSDisableScheduler raises) is held while the handler runs, so __OSReschedule at its end does
 * nothing and only RunQueueHint stays set. The switch then happens at the running thread's next
 * scheduling call (OSSleepThread, OSWakeupThread, OSYieldThread, mutexes...) - for the races
 * above that is the sleep itself, which is safe - or, if the thread keeps computing without one,
 * at a poll point at most RESCHED_FORCE_NS later. RT_NODEFER=1 restores the old behaviour.
 * Addresses: SelectThread's globals in the small-data area (r13): Reschedule at -20352,
 * RunQueueHint at -20348; __OSReschedule at 801B50D0 (USA rev 2 main.dol). */
#define SDA_RESCHEDULE   (-20352)
#define SDA_RUNQUEUEHINT (-20348)
#define A_OSRESCHEDULE   0x801B50D0u
#define RESCHED_FORCE_NS 1000000ull
static int defer_mode = -1;
static int sda_ok(uint32_t r13) { return r13 >= 0x80000000u + 0x8000u && r13 < 0x81800000u; }
uint64_t g_resched_deferred, g_resched_forced;              /* stats (hang reports) */
static uint64_t g_defer_since;                               /* when a switch was first postponed (0 = none pending) */

static void take_exception_at(ppc_context *ctx, uint8_t *base, int n, uint32_t handler);
static void take_exception(ppc_context *ctx, uint8_t *base, int n) {
    uint32_t handler = ld32(base, 0x80003000u + 4u * n);
    if (handler) take_exception_at(ctx, base, n, handler);
}
static void take_exception_at(ppc_context *ctx, uint8_t *base, int n, uint32_t handler) {
    if (cur->npoll == MAX_POLL) return;
    if (defer_mode < 0) { defer_mode = getenv("RT_NODEFER") == NULL; if (!defer_mode) rt_log("cpu", "deferred preemption off (RT_NODEFER)"); }
    uint32_t exc_ctx = ld32(base, 0x800000D4u);
    poll_frame *pf = &cur->polls[cur->npoll++];
    pf->exc_ctx = exc_ctx;
    pf->ctx = ctx;
    pf->snap = *ctx;
    pf->defer_sda = (defer_mode && sda_ok(ctx->r[13])) ? ctx->r[13] : 0;
    pf->switches = g_fiber_switches;
    if (pf->defer_sda) W32(pf->defer_sda + SDA_RESCHEDULE, R32(pf->defer_sda + SDA_RESCHEDULE) + 1);
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
        if (f->defer_sda) {
            W32(f->defer_sda + SDA_RESCHEDULE, R32(f->defer_sda + SDA_RESCHEDULE) - 1);
            if (R32(f->defer_sda + SDA_RUNQUEUEHINT)) { g_resched_deferred++; if (!g_defer_since) g_defer_since = rt_emu_ns(); }
        }
        {   /* RT_PREEMPTLOG=1: report interrupts that switched threads at a poll point (debug) */
            static int pl = -1; if (pl < 0) pl = getenv("RT_PREEMPTLOG") != NULL;
            if (pl && g_fiber_switches != f->switches) {
                static uint64_t n; n++;
                uint32_t lr = f->snap.lr;
                int hot = lr == 0x8016B514u || lr == 0x8016B7A8u;          /* mDvd waitDone / command thread windows */
                if (hot || n % 200 == 1) rt_log("preempt", "#%llu at poll after %08X (%s)%s", (unsigned long long)n, lr, rt_func_name(lr, NULL), hot ? "  <-- check-then-sleep race window" : "");
            }
        }
        *f->ctx = f->snap;
        t->npoll--;
    }
}


/* ------------------------------------------------------------------ native events (track A)
 * Native replacements of the SDK's device libraries (docs/NATIVE_ROADMAP.md) finish their work
 * at once, but the game expects completion callbacks "later, from an interrupt" - never inside
 * the call that started the work. rt_post_callback(fn, a0, a1) queues a guest callback; it runs
 * at the next point where the guest has interrupts enabled, in an interrupt-like frame (EE off,
 * scheduler switches deferred - the same frame take_exception builds for real interrupts). */
#define EV_MAX 128
static struct { uint32_t fn, a0, a1; } ev_q[EV_MAX]; static int ev_n;
#define EV_HANDLER 0x80001F00u                            /* (guest address of the native dispatcher) */
void rt_post_callback(uint32_t fn, uint32_t a0, uint32_t a1) {
    if (!fn) return;
    if (ev_n == EV_MAX) rt_fatal("native event queue full (callback %08X)", fn);
    ev_q[ev_n].fn = fn; ev_q[ev_n].a0 = a0; ev_q[ev_n].a1 = a1; ev_n++;
}
int rt_events_pending(void) { return ev_n; }
static void ev_dispatch(ppc_context *ctx, uint8_t *base) {
    while (ev_n) {
        uint32_t fn = ev_q[0].fn, a0 = ev_q[0].a0, a1 = ev_q[0].a1;
        memmove(&ev_q[0], &ev_q[1], sizeof ev_q[0] * (size_t)(ev_n - 1)); ev_n--;
        ctx->r[3] = a0; ctx->r[4] = a1; ctx->lr = 0;
        ppc_call_indirect(ctx, base, fn);
    }
}

/* a switch asked for by an interrupt that the running thread hasn't reached a scheduling call
   for: do it here, like the end of an interrupt handler would (interrupts off, registers kept) */
static void maybe_force_resched(ppc_context *ctx, uint8_t *base) {
    if (!g_defer_since) return;
    uint32_t sda = ctx->r[13];
    if (!sda_ok(sda) || cur->npoll) return;
    if (!R32(sda + SDA_RUNQUEUEHINT)) { g_defer_since = 0; return; }   /* the thread got there by itself */
    if ((int32_t)R32(sda + SDA_RESCHEDULE) > 0) return;               /* guest holds the scheduler lock */
    if (rt_emu_ns() - g_defer_since < RESCHED_FORCE_NS) return;
    g_defer_since = 0;
    ppc_context snap = *ctx;
    ctx->msr &= ~0x8000u;
    g_resched_forced++;
    ppc_call_indirect(ctx, base, A_OSRESCHEDULE);        /* may switch threads; returns when we run again */
    *ctx = snap;
}

/* Idle: when no guest thread can run, the OS spins in SelectThread's idle loop (current thread
   = NULL) until an interrupt makes one ready. Every interrupt source is either immediate (then
   pi_active() is already set) or timed, so instead of spinning a CPU core at 100 % the host
   thread sleeps until shortly before the next timed event. NSMBW_IDLESLEEP=0 keeps spinning. */
uint64_t hw_next_event_ns(void);
void plat_idle_wait(uint64_t ns);
uint64_t g_idle_sleep_ns RT_LOCAL;
/* deterministic mode: keep the virtual clock near the PC's (sleep while it is ahead) */
static uint64_t pace_wall0 RT_LOCAL, pace_vt0 RT_LOCAL;   /* (the PC's clock: after a load, pacing starts anew) */
void det_pace_reset(void) { pace_wall0 = 0; }
void det_pace_slow(uint64_t ns) { if (pace_wall0) pace_wall0 += ns; }   /* (online time sync: this PC runs ns later from now on) */
extern int g_det_fast;                              /* rt_net.c: catching up with the online game */
static void det_pace(void) {
    uint64_t wall = rt_now_ns();
    if (g_det_fast || g_resim) { pace_wall0 = 0; return; }   /* (unpaced; pacing starts anew afterwards) */
    if (!pace_wall0 || g_vt_ns < pace_vt0) { pace_wall0 = wall; pace_vt0 = g_vt_ns; }
    uint64_t target = pace_wall0 + (g_vt_ns - pace_vt0);
    if (wall > target + 100000000ull) { pace_wall0 += wall - target; return; }   /* (far behind: don't race to catch up) */
    if (target > wall + 300000ull) { uint64_t ns = target - wall; if (ns > 4000000ull) ns = 4000000ull; plat_idle_wait(ns); g_idle_sleep_ns += ns; }
}
static void idle_wait(ppc_context *ctx) {
    static int on = -1;
    if (g_det) {                                            /* nothing to run: on to the next timed event */
        if (R32(0x800000E4) != 0 || cur->npoll || pi_active() || ev_n) return;
        (void)ctx;
        uint64_t next = hw_next_event_ns();
        if (dec_armed) {
            int64_t dt = (int64_t)(dec_deadline - rt_tb_now());
            uint64_t dn = dt <= 0 ? g_vt_ns : g_vt_ns + (uint64_t)dt * 1000000000ull / TB_HZ + 1;
            if (dn < next) next = dn;
        }
        if (next > g_vt_ns) g_vt_ns = next;
        det_pace();
        return;
    }
    if (on < 0) { const char *e = getenv("NSMBW_IDLESLEEP"); on = !(e && *e == '0'); }
    if (!on || R32(0x800000E4) != 0 || cur->npoll || pi_active() || ev_n) return;
    (void)ctx;
    uint64_t now = rt_now_ns(), next = hw_next_event_ns();
    if (dec_armed) {
        int64_t dt = (int64_t)(dec_deadline - rt_tb_now());
        uint64_t dn = dt <= 0 ? now : now + (uint64_t)dt * 1000000000ull / TB_HZ;
        if (dn < next) next = dn;
    }
    if (next <= now + 1000000ull) return;                   /* less than 1 ms: keep spinning */
    uint64_t ns = next - now - 400000ull;                    /* wake a bit early */
    if (ns > 4000000ull) ns = 4000000ull;
    plat_idle_wait(ns);
    g_idle_sleep_ns += ns;
}

void *volatile g_poll_site RT_LOCAL; ppc_context *volatile g_poll_ctx RT_LOCAL;     /* hang reports: last poll point */
/* RT_POLLSTRESS=1 (testing): every loop back-edge is a poll point instead of every 256th, which
   makes timing races between guest threads show up in seconds instead of hours */
static void poll_stress(void) {
    static int on = -1; if (on < 0) on = getenv("RT_POLLSTRESS") != NULL;
    if (on) ppc_poll_counter = 0xFFFFFFFFu;
}
static uint64_t rt_hash64(const void *p, size_t n) { const uint8_t *b = p; uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } return h; }
void ppc_poll(ppc_context *ctx, uint8_t *base) {
    g_poll_site = __builtin_return_address(0); g_poll_ctx = ctx;
    poll_stress();
    if (g_det) {
        g_vt_ns += VT_POLL_NS;
        static int dbg = -1; static uint64_t last RT_LOCAL; if (dbg < 0) dbg = getenv("RT_DET_DEBUG") != NULL;
        if (dbg && rt_now_ns() - last > 2000000000ull) { extern uint64_t g_vi_fields; last = rt_now_ns();
            rt_log("det", "vt %.3f s, fields %llu, lr %08X (%s), msr %08X", (g_vt_ns - det_t0) / 1e9, (unsigned long long)g_vi_fields, ctx->lr, rt_func_name(ctx->lr, NULL), ctx->msr); }
    }
    {   static long lf = -2, lt; if (lf == -2) { const char *e = getenv("RT_POLL_LOG"); lf = -1; if (e) sscanf(e, "%ld,%ld", &lf, &lt); }   /* testing: every poll */
        extern uint64_t g_vi_fields;
        if (lf >= 0 && (long)g_vi_fields >= lf && (long)g_vi_fields <= lt)
            rt_log("poll", "f %llu vt %llu lr %08X r1 %08X r3 %08X t %d cnt %u regs %016llx", (unsigned long long)g_vi_fields, (unsigned long long)g_vt_ns, ctx->lr, ctx->r[1], ctx->r[3], cur == &main_thread ? -1 : (int)(cur - thread_pool), ppc_poll_counter, (unsigned long long)rt_hash64(ctx, sizeof *ctx)); }
    hw_update();
    if (!(ctx->msr & 0x8000u)) return;
    if (pi_active()) {
        {   static long lf = -2, lt; if (lf == -2) { const char *e = getenv("RT_POLL_LOG"); lf = -1; if (e) sscanf(e, "%ld,%ld", &lf, &lt); }
            extern uint64_t g_vi_fields; extern uint32_t pi_intsr_dbg(void), pi_intmr_dbg(void);
            if (lf >= 0 && (long)g_vi_fields >= lf && (long)g_vi_fields <= lt) rt_log("poll", "external: intsr %08X intmr %08X", pi_intsr_dbg(), pi_intmr_dbg()); }
        take_exception(ctx, base, EXC_EXTERNAL); return; }
    if (ev_n) {                                              /* native events (completion callbacks) */
        static int reg; if (!reg) { reg = 1; rt_register_native(EV_HANDLER, ev_dispatch); }
        take_exception_at(ctx, base, EXC_EXTERNAL, EV_HANDLER); return; }
    if (dec_armed && (int64_t)(rt_tb_now() - dec_deadline) >= 0) {
        dec_armed = 0;
        {   static int lg = -1; if (lg < 0) lg = getenv("RT_DEC_LOG") != NULL;     /* testing: decrementer timing */
            if (lg) { extern uint64_t g_vi_fields; rt_log("dec", "field %llu vt %llu tb %llu deadline %llu lr %08X", (unsigned long long)g_vi_fields, (unsigned long long)g_vt_ns, (unsigned long long)rt_tb_now(), (unsigned long long)dec_deadline, ctx->lr); } }
        take_exception(ctx, base, EXC_DECREMENTER);
        return;
    }
    if (defer_mode > 0) maybe_force_resched(ctx, base);
    idle_wait(ctx);
}


/* state for hang reports (printed by rt_dump_trace) */
void rt_hang_state(void) {
    extern uint32_t pi_intsr_dbg(void), pi_intmr_dbg(void);
    ppc_context *c = g_poll_ctx;
    fprintf(stderr, "cpu: fiber %p (main %p) npoll %d | last poll from host %p", (void *)cur, (void *)&main_thread, cur ? cur->npoll : -1, g_poll_site);
#ifdef _WIN32
    fprintf(stderr, " (image+0x%llX)", (unsigned long long)((uintptr_t)g_poll_site - (uintptr_t)GetModuleHandleA(NULL)));
#endif
    if (c) {
        fprintf(stderr, "\n     msr %08X lr %08X (%s) r1 %08X r3 %08X r13 %08X", c->msr, c->lr, rt_func_name(c->lr, NULL), c->r[1], c->r[3], c->r[13]);
        if (sda_ok(c->r[13]))                          /* (never read MMIO from here: this may run on the watchdog thread) */
            fprintf(stderr, "\n     RunQueueBits %08X Reschedule %d RunQueueHint %d current ctx %08X",
                    R32(c->r[13] - 20344), (int)R32(c->r[13] - 20352), (int)R32(c->r[13] - 20348), R32(0x800000D4));
    }
    fprintf(stderr, "\n     PI intsr %08X intmr %08X | dec armed %d in %lld ticks | preemptions deferred %llu, forced %llu\n", pi_intsr_dbg(), pi_intmr_dbg(), dec_armed,
            (long long)(dec_deadline - rt_tb_now()), (unsigned long long)g_resched_deferred, (unsigned long long)g_resched_forced);
    if (c && sda_ok(c->r[13])) {                       /* the game's disc command queue (mDvd): r13-21076 = manager */
        uint32_t mgr = R32(c->r[13] - 21076);
        if (mgr >= 0x80000000u && mgr < 0x81800000u) fprintf(stderr, "     mDvd command queue: first %08X last %08X\n", R32(mgr + 24), R32(mgr + 28));
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
    { char sp[1200]; size_t sl; snprintf(sp, sizeof sp, "%s/title/00000001/00000002/data/setting.txt", rt_nand_root());
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

static void guest_boot(void *arg) {
    (void)arg;
    ppc_context *ctx = &main_thread.ctx;
    ctx->msr = 0x00002032;                 /* FP, IR, DR, RI */
    ctx->r[1] = 0x816FFFF0;
    rt_log("boot", "jumping to __start");
    ppc_call_indirect(ctx, g_base, 0x80004050);
    rt_fatal("__start returned");
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
    g_base = rt_layout_init();                     /* (fixed addresses: rt_state.c) */
    if (!g_base) rt_fatal("cannot allocate guest memory");
    t0_ns = rt_now_ns();
    {   const char *e = getenv("RT_DETERMINISTIC");
        if (e && *e != '0') { g_det = 1; det_t0 = g_vt_ns; rt_log("cpu", "deterministic mode: virtual clock (%llu ns per poll)", (unsigned long long)VT_POLL_NS); } }
    plat_init();
    update_install_pending();                      /* (an update downloaded last time: before anything opens its files) */
    update_begin();
    load_dol();
    void mod_load(void); mod_load();
    if (disc_init()) rt_fatal("cannot open disc files in %s", g_disc);
    setup_low_memory();
    hw_init(); ios_init(); gx_init();
    /* the guest's main thread runs on a fiber of its own (rt_state.c: its stack is part of a
       save state); main's stack stays the host fiber, where save states are loaded */
    plat_fiber_main();
    main_thread.fiber = plat_fiber_create(guest_boot, NULL);
    cur = &main_thread;
    rt_state_host_loop(main_thread.fiber);
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
