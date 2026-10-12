/* ppc_context.h - guest CPU state and helpers for statically recompiled Gekko (Wii) code.
 *
 * Guest memory model: `base` points at a 1 GB host region indexed by *physical* address
 * (guest virtual & 0x3FFFFFFF). That folds the cached/uncached mirrors together
 * (0x80/0xC0 -> MEM1 at 0, 0x90/0xD0 -> MEM2 at 0x10000000, 0xE0 -> locked cache at
 * 0x20000000), exactly like the BATs the Wii OS sets up. Physical 0x0C000000-0x0DFFFFFF
 * is hardware registers and is routed to the runtime's MMIO handlers. Guest memory is
 * big-endian, so every load and store byte-swaps.
 */
#ifndef PPC_CONTEXT_H
#define PPC_CONTEXT_H
#include <stdint.h>
#include <string.h>
#include <math.h>

#if defined(_MSC_VER)
#include <stdlib.h>
#define BSWAP16 _byteswap_ushort
#define BSWAP32 _byteswap_ulong
#define BSWAP64 _byteswap_uint64
#define PPC_INLINE static __forceinline
#else
#define BSWAP16 __builtin_bswap16
#define BSWAP32 __builtin_bswap32
#define BSWAP64 __builtin_bswap64
#define PPC_INLINE static inline __attribute__((always_inline))
#endif

typedef union { double d; uint64_t u; } ppc_fpr;

typedef struct ppc_context {
    uint32_t r[32];
    ppc_fpr  f[32];      /* ps0 */
    double   ps1[32];    /* paired-single slot 1 */
    uint32_t cr;         /* CR0 in bits 31..28 */
    uint32_t lr, ctr;
    uint32_t xer_so, xer_ov, xer_ca, xer_bc;
    uint32_t fpscr;
    uint32_t msr;
    uint32_t gqr[8];
    uint32_t spr[1024];  /* everything else (HID0/2, SRR0/1, SPRG, WPAR, DMA...) */
    uint32_t reserve;    /* lwarx/stwcx. reservation */
} ppc_context;

typedef void (*ppc_func)(ppc_context *ctx, uint8_t *base);

/* ---- runtime services implemented outside generated code ---- */
void ppc_call_indirect(ppc_context *ctx, uint8_t *base, uint32_t addr);
void ppc_unimplemented(ppc_context *ctx, uint8_t *base, uint32_t addr, const char *what);
void ppc_syscall(ppc_context *ctx, uint8_t *base, uint32_t addr);
void ppc_trap(ppc_context *ctx, uint8_t *base, uint32_t addr);
uint64_t ppc_timebase(void);

/* ---- memory ---- */
#define PPC_PHYS(a) ((uint32_t)(a) & 0x3FFFFFFFu)
#define PPC_IS_MMIO(p) (((p) >> 25) == 6u)          /* 0x0C000000 - 0x0DFFFFFF */
uint32_t mmio_read(uint32_t phys, int size);
void     mmio_write(uint32_t phys, uint32_t v, int size);

PPC_INLINE uint8_t  ld8 (uint8_t *b, uint32_t a) { uint32_t p = PPC_PHYS(a); if (PPC_IS_MMIO(p)) return (uint8_t)mmio_read(p, 1); return b[p]; }
PPC_INLINE uint16_t ld16(uint8_t *b, uint32_t a) { uint32_t p = PPC_PHYS(a); if (PPC_IS_MMIO(p)) return (uint16_t)mmio_read(p, 2); uint16_t v; memcpy(&v, b + p, 2); return BSWAP16(v); }
PPC_INLINE uint32_t ld32(uint8_t *b, uint32_t a) { uint32_t p = PPC_PHYS(a); if (PPC_IS_MMIO(p)) return mmio_read(p, 4); uint32_t v; memcpy(&v, b + p, 4); return BSWAP32(v); }
PPC_INLINE uint64_t ld64(uint8_t *b, uint32_t a) { uint32_t p = PPC_PHYS(a); uint64_t v; memcpy(&v, b + p, 8); return BSWAP64(v); }
PPC_INLINE void st8 (uint8_t *b, uint32_t a, uint8_t  v) { uint32_t p = PPC_PHYS(a); if (PPC_IS_MMIO(p)) { mmio_write(p, v, 1); return; } b[p] = v; }
PPC_INLINE void st16(uint8_t *b, uint32_t a, uint16_t v) { uint32_t p = PPC_PHYS(a); if (PPC_IS_MMIO(p)) { mmio_write(p, v, 2); return; } v = BSWAP16(v); memcpy(b + p, &v, 2); }
PPC_INLINE void st32(uint8_t *b, uint32_t a, uint32_t v) { uint32_t p = PPC_PHYS(a); if (PPC_IS_MMIO(p)) { mmio_write(p, v, 4); return; } v = BSWAP32(v); memcpy(b + p, &v, 4); }
PPC_INLINE void st64(uint8_t *b, uint32_t a, uint64_t v) {
    uint32_t p = PPC_PHYS(a);
    if (PPC_IS_MMIO(p)) { mmio_write(p, (uint32_t)(v >> 32), 4); mmio_write(p, (uint32_t)v, 4); return; }
    v = BSWAP64(v); memcpy(b + p, &v, 8);
}
PPC_INLINE void ppc_dcbz(uint8_t *b, uint32_t a) { memset(b + (PPC_PHYS(a) & ~31u), 0, 32); }

/* ---- interrupts, timers, tracing (runtime) ---- */
extern volatile uint32_t ppc_poll_counter;
void ppc_poll(struct ppc_context *ctx, uint8_t *base);
void ppc_rfi(struct ppc_context *ctx, uint8_t *base, uint32_t pc);
uint32_t ppc_get_dec(struct ppc_context *ctx);
void ppc_set_dec(struct ppc_context *ctx, uint32_t v);
void ppc_set_tb(struct ppc_context *ctx, int upper, uint32_t v);
void ppc_set_dma_l(struct ppc_context *ctx, uint8_t *base, uint32_t v);
/* backward branches: give pending interrupts a chance every 256 loop iterations */
#define PPC_POLL() do { if (((++ppc_poll_counter) & 0xFF) == 0 && (ctx->msr & 0x8000u)) ppc_poll(ctx, base); } while (0)
#ifdef PPC_TRACE
extern uint32_t ppc_trace_ring[256]; extern uint32_t ppc_trace_pos;
#define PPC_ENTER(a) (ppc_trace_ring[(ppc_trace_pos++) & 255] = (a))
#else
#define PPC_ENTER(a) ((void)0)
#endif

PPC_INLINE float  u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
PPC_INLINE uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
PPC_INLINE double ldfs(uint8_t *b, uint32_t a) { return (double)u2f(ld32(b, a)); }
PPC_INLINE void   stfs(uint8_t *b, uint32_t a, double d) { st32(b, a, f2u((float)d)); }

/* ---- integer helpers ---- */
PPC_INLINE uint32_t rotl32(uint32_t x, uint32_t n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
PPC_INLINE uint32_t cntlzw(uint32_t x) {
#if defined(_MSC_VER)
    unsigned long i; return _BitScanReverse(&i, x) ? 31 - i : 32;
#else
    return x ? (uint32_t)__builtin_clz(x) : 32;
#endif
}

/* CR field n (0..7) <- 4-bit value (lt gt eq so) */
#define CR_SET(ctx, n, v) ((ctx)->cr = ((ctx)->cr & ~(0xFu << (28 - 4 * (n)))) | ((uint32_t)(v) << (28 - 4 * (n))))
#define CR_BIT(ctx, i)    (((ctx)->cr >> (31 - (i))) & 1u)
#define CR_SETBIT(ctx, i, v) ((ctx)->cr = ((ctx)->cr & ~(1u << (31 - (i)))) | ((uint32_t)((v) & 1) << (31 - (i))))

PPC_INLINE void cr_cmp_s(ppc_context *c, int n, int32_t a, int32_t b) {
    CR_SET(c, n, ((a < b) << 3) | ((a > b) << 2) | ((a == b) << 1) | (c->xer_so & 1));
}
PPC_INLINE void cr_cmp_u(ppc_context *c, int n, uint32_t a, uint32_t b) {
    CR_SET(c, n, ((a < b) << 3) | ((a > b) << 2) | ((a == b) << 1) | (c->xer_so & 1));
}
#define CR0(c, v) cr_cmp_s((c), 0, (int32_t)(v), 0)

PPC_INLINE void cr_fcmp(ppc_context *c, int n, double a, double b) {
    uint32_t v = (a != a || b != b) ? 1 : (a < b) ? 8 : (a > b) ? 4 : 2;
    CR_SET(c, n, v);
    c->fpscr = (c->fpscr & ~0xF000u) | (v << 12);
}

PPC_INLINE uint32_t mask32(uint32_t mb, uint32_t me) {
    uint32_t m1 = 0xFFFFFFFFu >> mb, m2 = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (m1 & m2) : (m1 | m2);
}

PPC_INLINE uint32_t divw(int32_t a, int32_t b) {
    if (b == 0 || (a == INT32_MIN && b == -1)) return a < 0 ? 0xFFFFFFFFu : 0;
    return (uint32_t)(a / b);
}
PPC_INLINE uint32_t divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }

/* fctiw/fctiwz: saturating double -> int32, result in low word of the FPR */
PPC_INLINE uint64_t fctiw_bits(double d, int truncate) {
    int32_t i;
    if (d != d) i = INT32_MIN;
    else if (d >= 2147483647.0) i = INT32_MAX;
    else if (d <= -2147483648.0) i = INT32_MIN;
    else i = (int32_t)(truncate ? trunc(d) : nearbyint(d));
    return 0xFFF8000000000000ull | (uint32_t)i;
}

PPC_INLINE double frsp(double d) { return (double)(float)d; }
PPC_INLINE double fres(double d) { return (double)(float)(1.0 / d); }
PPC_INLINE double frsqrte(double d) { return 1.0 / sqrt(d); }

/* ---- paired-single quantized load/store (GQR-driven) ---- */
PPC_INLINE double psq_dequant(uint8_t *b, uint32_t a, uint32_t type, uint32_t scale, uint32_t *sz) {
    double s = ldexp(1.0, -(int32_t)((int8_t)(scale << 2) >> 2));
    switch (type) {
    case 4: *sz = 1; return (double)ld8(b, a) * s;
    case 5: *sz = 2; return (double)ld16(b, a) * s;
    case 6: *sz = 1; return (double)(int8_t)ld8(b, a) * s;
    case 7: *sz = 2; return (double)(int16_t)ld16(b, a) * s;
    default: *sz = 4; return ldfs(b, a);
    }
}
PPC_INLINE void psq_quant(uint8_t *b, uint32_t a, double v, uint32_t type, uint32_t scale, uint32_t *sz) {
    double s = ldexp(1.0, (int32_t)((int8_t)(scale << 2) >> 2));
    double x = v * s;
    switch (type) {
    case 4: *sz = 1; st8(b, a, (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x)); break;
    case 5: *sz = 2; st16(b, a, (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x)); break;
    case 6: *sz = 1; st8(b, a, (uint8_t)(int8_t)(x < -128 ? -128 : x > 127 ? 127 : x)); break;
    case 7: *sz = 2; st16(b, a, (uint16_t)(int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x)); break;
    default: *sz = 4; stfs(b, a, v); break;
    }
}
PPC_INLINE void psq_l(ppc_context *c, uint8_t *b, int fd, uint32_t a, int w, int i) {
    uint32_t g = c->gqr[i], type = (g >> 16) & 7, scale = (g >> 24) & 0x3F, sz;
    c->f[fd].d = psq_dequant(b, a, type, scale, &sz);
    c->ps1[fd] = w ? 1.0 : psq_dequant(b, a + sz, type, scale, &sz);
}
PPC_INLINE void psq_st(ppc_context *c, uint8_t *b, int fs, uint32_t a, int w, int i) {
    uint32_t g = c->gqr[i], type = g & 7, scale = (g >> 8) & 0x3F, sz;
    psq_quant(b, a, c->f[fs].d, type, scale, &sz);
    if (!w) psq_quant(b, a + sz, c->ps1[fs], type, scale, &sz);
}

#endif
