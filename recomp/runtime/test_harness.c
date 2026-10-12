/* test_harness.c - runs recompiled NSMBW library functions on the host and checks results
 * against host-native reference implementations. Linux/POSIX (mmap) for now. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/mman.h>
#include "recomp_funcs.h"

static int fails = 0, passes = 0, unimpl_hits = 0;
static long indirect_calls = 0;
#define CHECK(cond, ...) do { if (cond) passes++; else { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- runtime services required by generated code ---- */
static ppc_func lookup(uint32_t addr) {
    uint32_t lo = 0, hi = ppc_func_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (ppc_func_table[mid].addr == addr) return ppc_func_table[mid].fn;
        if (ppc_func_table[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
void ppc_call_indirect(ppc_context *ctx, uint8_t *base, uint32_t addr) {
    ppc_func f = lookup(addr);
    if (!f) { printf("  [runtime] indirect call to unknown address %08X\n", addr); unimpl_hits++; return; }
    indirect_calls++;
    f(ctx, base);
}
void ppc_unimplemented(ppc_context *ctx, uint8_t *base, uint32_t addr, const char *what) {
    (void)ctx; (void)base; printf("  [runtime] unimplemented %s at %08X\n", what, addr); unimpl_hits++;
}
void ppc_syscall(ppc_context *ctx, uint8_t *base, uint32_t addr) { (void)ctx; (void)base; printf("  [runtime] sc at %08X\n", addr); }
void ppc_trap(ppc_context *ctx, uint8_t *base, uint32_t addr) { (void)ctx; (void)base; printf("  [runtime] trap at %08X\n", addr); }
uint64_t ppc_timebase(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 60750000ull + t.tv_nsec * 60750000ull / 1000000000ull; }

/* ---- guest setup ---- */
static uint8_t *base;
static ppc_context ctx;

static void load_dol(const char *path) {
    FILE *fp = fopen(path, "rb"); if (!fp) { perror(path); exit(1); }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *d = malloc(n); if (fread(d, 1, n, fp) != (size_t)n) exit(1); fclose(fp);
    for (int i = 0; i < 18; i++) {
        uint32_t off = BSWAP32(((uint32_t *)d)[i]), addr = BSWAP32(((uint32_t *)d)[18 + i]), sz = BSWAP32(((uint32_t *)d)[36 + i]);
        if (sz) memcpy(base + addr, d + off, sz);
    }
    uint32_t bss = BSWAP32(((uint32_t *)d)[54]), bss_sz = BSWAP32(((uint32_t *)d)[55]);
    (void)bss; (void)bss_sz; /* bss is zero from mmap; small-data sections inside it already zero */
    free(d);
}

static void call(uint32_t addr) {
    ppc_func f = lookup(addr);
    if (!f) { printf("no function at %08X\n", addr); exit(1); }
    ctx.lr = 0;
    f(&ctx, base);
}

#define HEAP 0x81200000u
static void put_str(uint32_t a, const char *s) { memcpy(base + a, s, strlen(s) + 1); }

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s main.dol [linked-dir]\n", argv[0]); return 1; }
    base = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    load_dol(argv[1]);
    memset(&ctx, 0, sizeof ctx);
    call(0x800041C0); /* __init_registers: sets r1 (stack), r2, r13 (small-data bases) */
    printf("after __init_registers: r1=%08X r2=%08X r13=%08X\n", ctx.r[1], ctx.r[2], ctx.r[13]);
    CHECK(ctx.r[1] >= 0x80000000u && ctx.r[2] && ctx.r[13], "init registers");
    uint32_t sp = ctx.r[1], r2 = ctx.r[2], r13 = ctx.r[13];
#define RESET() do { ctx.r[1] = sp; ctx.r[2] = r2; ctx.r[13] = r13; } while (0)

    /* memset / memcpy / strlen / strcmp / strcpy */
    RESET(); ctx.r[3] = HEAP; ctx.r[4] = 0xAB; ctx.r[5] = 1000; call(0x800046B4);
    int ok = 1; for (int i = 0; i < 1000; i++) ok &= base[HEAP + i] == 0xAB;
    CHECK(ok && base[HEAP + 1000] == 0, "memset");
    for (int i = 0; i < 777; i++) base[HEAP + 0x1000 + i] = (uint8_t)(i * 7 + 3);
    RESET(); ctx.r[3] = HEAP + 0x2003; ctx.r[4] = HEAP + 0x1000; ctx.r[5] = 777; call(0x80004364);
    CHECK(memcmp(base + HEAP + 0x2003, base + HEAP + 0x1000, 777) == 0, "memcpy (unaligned dst)");
    put_str(HEAP + 0x3000, "New Super Mario Bros. Wii");
    RESET(); ctx.r[3] = HEAP + 0x3000; call(0x802DC69C);
    CHECK(ctx.r[3] == 25, "strlen -> %u", ctx.r[3]);
    const char *pairs[][2] = { {"abc", "abd"}, {"mario", "mario"}, {"luigi", "luig"}, {"", "a"}, {"Toad", "toad"} };
    for (int i = 0; i < 5; i++) {
        put_str(HEAP + 0x3100, pairs[i][0]); put_str(HEAP + 0x3200, pairs[i][1]);
        RESET(); ctx.r[3] = HEAP + 0x3100; ctx.r[4] = HEAP + 0x3200; call(0x802E1AB4);
        int g = (int32_t)ctx.r[3], h = strcmp(pairs[i][0], pairs[i][1]);
        CHECK((g > 0) == (h > 0) && (g < 0) == (h < 0), "strcmp(%s,%s) guest=%d host=%d", pairs[i][0], pairs[i][1], g, h);
    }
    RESET(); ctx.r[3] = HEAP + 0x3300; ctx.r[4] = HEAP + 0x3000; call(0x802E1938);
    CHECK(strcmp((char *)base + HEAP + 0x3300, "New Super Mario Bros. Wii") == 0, "strcpy");
    put_str(HEAP + 0x3400, "  -12345xyz");
    RESET(); ctx.r[3] = HEAP + 0x3400; call(0x802E2108);
    CHECK((int32_t)ctx.r[3] == -12345, "atoi -> %d", (int32_t)ctx.r[3]);

    /* 64-bit runtime helpers: carry chains, shifts, cntlzw, divwu */
    srand(1234); int divok = 1;
    for (int i = 0; i < 20000; i++) {
        uint64_t a = ((uint64_t)rand() << 40) ^ ((uint64_t)rand() << 20) ^ rand();
        uint64_t b = (i & 1) ? (uint64_t)rand() + 1 : (((uint64_t)rand() << 31) ^ rand()) + 1;
        RESET(); ctx.r[3] = a >> 32; ctx.r[4] = (uint32_t)a; ctx.r[5] = b >> 32; ctx.r[6] = (uint32_t)b; call(0x802DCDD8);
        uint64_t q = ((uint64_t)ctx.r[3] << 32) | ctx.r[4];
        RESET(); ctx.r[3] = a >> 32; ctx.r[4] = (uint32_t)a; ctx.r[5] = b >> 32; ctx.r[6] = (uint32_t)b; call(0x802DCFFC);
        uint64_t m = ((uint64_t)ctx.r[3] << 32) | ctx.r[4];
        if (q != a / b || m != a % b) { if (divok) printf("  div2u/mod2u mismatch %llx / %llx: q=%llx m=%llx\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)q, (unsigned long long)m); divok = 0; }
    }
    CHECK(divok, "__div2u/__mod2u on 20000 random 64-bit pairs");
    RESET(); ctx.r[3] = 0x00000001; ctx.r[4] = 0x80000001; ctx.r[5] = 7; call(0x802DD1EC);
    CHECK((((uint64_t)ctx.r[3] << 32) | ctx.r[4]) == (0x0000000180000001ull << 7), "__shl2i");

    /* floating point: fdlibm sin/cos/floor/ceil, fp->unsigned conversion */
    double maxerr = 0; int fpok = 1;
    for (int i = -200; i <= 200; i++) {
        double x = i * 0.0731;
        RESET(); ctx.f[1].d = x; call(0x802E84C4); maxerr = fmax(maxerr, fabs(ctx.f[1].d - sin(x)));
        RESET(); ctx.f[1].d = x; call(0x802E7FBC); maxerr = fmax(maxerr, fabs(ctx.f[1].d - cos(x)));
        RESET(); ctx.f[1].d = x * 3.3; call(0x802E8084); fpok &= ctx.f[1].d == floor(x * 3.3);
        RESET(); ctx.f[1].d = x * 3.3; call(0x802E7E44); fpok &= ctx.f[1].d == ceil(x * 3.3);
    }
    CHECK(maxerr < 1e-15, "sin/cos max error %.3g", maxerr);
    CHECK(fpok, "floor/ceil exact");
    RESET(); ctx.f[1].d = 3000000000.75; call(0x802DCC4C);
    CHECK(ctx.r[3] == 3000000000u, "__cvt_fp2unsigned -> %u", ctx.r[3]);

    /* paired-single math library */
    float A[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, B[12] = {0.5f, -1, 2, 0.25f, 3, 0, -2, 1, 1.5f, 2.5f, -0.5f, 4};
    for (int i = 0; i < 12; i++) { st32(base, HEAP + 0x4000 + i * 4, f2u(A[i])); st32(base, HEAP + 0x4100 + i * 4, f2u(B[i])); }
    RESET(); ctx.r[3] = HEAP + 0x4000; ctx.r[4] = HEAP + 0x4100; ctx.r[5] = HEAP + 0x4200; call(0x801C0540);
    int mok = 1;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) {
        float e = A[r * 4 + 0] * B[0 * 4 + c] + A[r * 4 + 1] * B[1 * 4 + c] + A[r * 4 + 2] * B[2 * 4 + c] + (c == 3 ? A[r * 4 + 3] : 0);
        float g = u2f(ld32(base, HEAP + 0x4200 + (r * 4 + c) * 4));
        if (fabsf(g - e) > 1e-4f) { mok = 0; printf("  PSMTXConcat[%d][%d] guest=%g expect=%g\n", r, c, g, e); }
    }
    CHECK(mok, "PSMTXConcat (paired singles, psq_l/psq_st)");
    float v[3] = {3, 4, 12};
    for (int i = 0; i < 3; i++) st32(base, HEAP + 0x4300 + i * 4, f2u(v[i]));
    RESET(); ctx.r[3] = HEAP + 0x4300; call(0x801C14C0);
    CHECK(fabs(ctx.f[1].d - 13.0) < 1e-3, "PSVECMag(3,4,12) -> %g", ctx.f[1].d);
    RESET(); ctx.r[3] = HEAP + 0x4300; ctx.r[4] = HEAP + 0x4300; call(0x801C1510);
    CHECK(fabs(ctx.f[1].d - 169.0) < 1e-3, "PSVECDotProduct -> %g", ctx.f[1].d);
    RESET(); ctx.r[3] = HEAP + 0x4300; ctx.r[4] = HEAP + 0x4400; call(0x801C1470);
    float nx = u2f(ld32(base, HEAP + 0x4400)), ny = u2f(ld32(base, HEAP + 0x4404)), nz = u2f(ld32(base, HEAP + 0x4408));
    CHECK(fabsf(nx - 3 / 13.f) < 1e-3f && fabsf(ny - 4 / 13.f) < 1e-3f && fabsf(nz - 12 / 13.f) < 1e-3f, "PSVECNormalize -> %g %g %g", nx, ny, nz);

    /* sprintf: varargs, jump-table switch, integer + float formatting */
    put_str(HEAP + 0x5000, "World %d-%d: %s x%03d (%X) %.3f");
    put_str(HEAP + 0x5100, "Peach's Castle");
    RESET();
    ctx.r[3] = HEAP + 0x5200; ctx.r[4] = HEAP + 0x5000; ctx.r[5] = 8; ctx.r[6] = (uint32_t)-1; ctx.r[7] = HEAP + 0x5100; ctx.r[8] = 42; ctx.r[9] = 0xBEEF;
    ctx.f[1].d = 3.14159265; CR_SETBIT(&ctx, 6, 1); /* CR6 bit: FP args present in registers */
    call(0x802E17DC);
    const char *got = (const char *)base + HEAP + 0x5200;
    char want[128]; snprintf(want, sizeof want, "World %d-%d: %s x%03d (%X) %.3f", 8, -1, "Peach's Castle", 42, 0xBEEF, 3.14159265);
    CHECK(strcmp(got, want) == 0, "sprintf -> \"%s\" (expected \"%s\")", got, want);

    /* REL modules: load the pre-linked images and run each module's _prolog (static
     * constructors through relocated pointer tables, cross-module and into main.dol) */
    if (argc >= 3) {
        char path[1024]; snprintf(path, sizeof path, "%s/load.txt", argv[2]);
        FILE *lf = fopen(path, "r");
        if (!lf) { perror(path); return 1; }
        char name[64], file[128]; unsigned load, prolog, epilog;
        struct { char name[64]; unsigned prolog; } mods[8]; int nm = 0;
        while (nm < 8 && fscanf(lf, "%63s %127s %x %x %x", name, file, &load, &prolog, &epilog) == 5) {
            snprintf(path, sizeof path, "%s/%s", argv[2], file);
            FILE *mf = fopen(path, "rb"); if (!mf) { perror(path); return 1; }
            size_t n = fread(base + load, 1, 64u << 20, mf); fclose(mf);
            printf("loaded %-12s at %08X (%zu bytes)\n", name, load, n);
            snprintf(mods[nm].name, 64, "%s", name); mods[nm++].prolog = prolog;
        }
        fclose(lf);
        for (int i = 0; i < nm; i++) {
            RESET();
            for (int r = 14; r < 32; r++) ctx.r[r] = 0xC0DE0000u + r;
            int before = unimpl_hits; long ic0 = indirect_calls;
            call(mods[i].prolog);
            int saved = 1; for (int r = 14; r < 32; r++) saved &= ctx.r[r] == 0xC0DE0000u + r;
            printf("  %-12s _prolog ran %ld static constructors (indirect calls)\n", mods[i].name, indirect_calls - ic0);
            CHECK(ctx.r[1] == sp && saved && unimpl_hits == before,
                  "%s _prolog: stack %s, r14-r31 %s, %d runtime stubs", mods[i].name,
                  ctx.r[1] == sp ? "balanced" : "UNBALANCED", saved ? "preserved" : "CLOBBERED", unimpl_hits - before);
        }
    }

    printf("\n%d passed, %d failed, %d runtime stubs hit\n", passes, fails, unimpl_hits);
    return fails != 0;
}
