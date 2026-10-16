/* rt_state.c - fixed memory layout, guest thread fibers, save states.
 *
 * A save state is everything the running game is: guest memory (MEM1, MEM2, the locked cache),
 * ARAM, the runtime's own emulation state (the variables of the files marked RT_STATE_FILE,
 * plus single variables marked RT_STATE elsewhere) and the guest threads' native stacks - a
 * guest thread is a fiber running recompiled code, so where it is lives on its stack.
 *
 * For a state to be loadable at all, and in another copy of the game (online play: the host's
 * running game is sent to everyone), every address in it must mean the same thing in both
 * processes: guest memory and the fiber stacks are mapped at fixed addresses (RT_GUEST_VA,
 * RT_ARENA_VA), the exe is linked at a fixed base without ASLR (build.py), the guest threads'
 * records are static arrays, and the fibers are our own (a few instructions of assembly), not
 * the OS's. Variables in state files that belong to this process only (handles, host pointers,
 * the PC's clock, caches of the renderer) are marked RT_LOCAL and are left alone by a load.
 *
 * Taking a state happens on the running guest thread (rt_snap_take: like setjmp, it returns 0
 * after saving and 1 when a load resumes it). Loading can't run on a stack it overwrites, so
 * rt_snap_load switches to the process's own stack (the host fiber, where main() waits), copies
 * everything in and jumps to the point the state was taken at. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "rt.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <errno.h>
#endif

#define RT_GUEST_VA  0x500000000000ull
#define RT_ARENA_VA  0x600000000000ull
#define ARAM_SIZE    (16u << 20)
#define STACKS_OFF   (64ull << 20)
#define STACK_STRIDE (80ull << 20)
#define MAIN_STACK   (64ull << 20)
#define THREAD_STACK (16ull << 20)
#define MAXFIB 48

/* ---- fibers ---- */
struct rt_fiber { void *sp; uint8_t *lo, *hi; void (*fn)(void *); void *arg; int slot; };
static rt_fiber fibers[MAXFIB] RT_STATE;
static int nfib RT_STATE;
static rt_fiber *fib_cur RT_STATE;
static rt_fiber host_fiber RT_LOCAL;
static int layout_ok RT_LOCAL;

void rt_fsw(void **save_sp, void *new_sp);
void rt_fboot(void);
typedef struct { uint64_t q[32]; } rt_jb;
int rt_ssave(rt_jb *b);
void rt_sjump(void **save_sp, rt_jb *b);

#if defined(_WIN32)
__asm__(
    ".text\n.globl rt_fsw\nrt_fsw:\n"
    "  pushq %rbp\n  pushq %rbx\n  pushq %rdi\n  pushq %rsi\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  subq $168, %rsp\n"
    "  movdqu %xmm6, 0(%rsp)\n  movdqu %xmm7, 16(%rsp)\n  movdqu %xmm8, 32(%rsp)\n  movdqu %xmm9, 48(%rsp)\n  movdqu %xmm10, 64(%rsp)\n"
    "  movdqu %xmm11, 80(%rsp)\n  movdqu %xmm12, 96(%rsp)\n  movdqu %xmm13, 112(%rsp)\n  movdqu %xmm14, 128(%rsp)\n  movdqu %xmm15, 144(%rsp)\n"
    "  movq %rsp, (%rcx)\n  movq %rdx, %rsp\n"
    "  movdqu 0(%rsp), %xmm6\n  movdqu 16(%rsp), %xmm7\n  movdqu 32(%rsp), %xmm8\n  movdqu 48(%rsp), %xmm9\n  movdqu 64(%rsp), %xmm10\n"
    "  movdqu 80(%rsp), %xmm11\n  movdqu 96(%rsp), %xmm12\n  movdqu 112(%rsp), %xmm13\n  movdqu 128(%rsp), %xmm14\n  movdqu 144(%rsp), %xmm15\n"
    "  addq $168, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rsi\n  popq %rdi\n  popq %rbx\n  popq %rbp\n  ret\n"
    ".globl rt_fboot\nrt_fboot:\n  movq %rbx, %rcx\n  subq $32, %rsp\n  callq *%r12\n  ud2\n"
    /* rt_ssave(b): rbx rbp rdi rsi r12-r15, rsp after return, return address, xmm6-15 */
    ".globl rt_ssave\nrt_ssave:\n"
    "  movq %rbx, 0(%rcx)\n  movq %rbp, 8(%rcx)\n  movq %rdi, 16(%rcx)\n  movq %rsi, 24(%rcx)\n"
    "  movq %r12, 32(%rcx)\n  movq %r13, 40(%rcx)\n  movq %r14, 48(%rcx)\n  movq %r15, 56(%rcx)\n"
    "  leaq 8(%rsp), %rdx\n  movq %rdx, 64(%rcx)\n  movq (%rsp), %rdx\n  movq %rdx, 72(%rcx)\n"
    "  movdqu %xmm6, 80(%rcx)\n  movdqu %xmm7, 96(%rcx)\n  movdqu %xmm8, 112(%rcx)\n  movdqu %xmm9, 128(%rcx)\n  movdqu %xmm10, 144(%rcx)\n"
    "  movdqu %xmm11, 160(%rcx)\n  movdqu %xmm12, 176(%rcx)\n  movdqu %xmm13, 192(%rcx)\n  movdqu %xmm14, 208(%rcx)\n  movdqu %xmm15, 224(%rcx)\n"
    "  xorl %eax, %eax\n  ret\n"
    /* rt_sjump(save_sp, b): suspend this stack like rt_fsw, then resume b with 1 */
    ".globl rt_sjump\nrt_sjump:\n"
    "  pushq %rbp\n  pushq %rbx\n  pushq %rdi\n  pushq %rsi\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  subq $168, %rsp\n"
    "  movdqu %xmm6, 0(%rsp)\n  movdqu %xmm7, 16(%rsp)\n  movdqu %xmm8, 32(%rsp)\n  movdqu %xmm9, 48(%rsp)\n  movdqu %xmm10, 64(%rsp)\n"
    "  movdqu %xmm11, 80(%rsp)\n  movdqu %xmm12, 96(%rsp)\n  movdqu %xmm13, 112(%rsp)\n  movdqu %xmm14, 128(%rsp)\n  movdqu %xmm15, 144(%rsp)\n"
    "  movq %rsp, (%rcx)\n"
    "  movq 0(%rdx), %rbx\n  movq 8(%rdx), %rbp\n  movq 16(%rdx), %rdi\n  movq 24(%rdx), %rsi\n"
    "  movq 32(%rdx), %r12\n  movq 40(%rdx), %r13\n  movq 48(%rdx), %r14\n  movq 56(%rdx), %r15\n"
    "  movdqu 80(%rdx), %xmm6\n  movdqu 96(%rdx), %xmm7\n  movdqu 112(%rdx), %xmm8\n  movdqu 128(%rdx), %xmm9\n  movdqu 144(%rdx), %xmm10\n"
    "  movdqu 160(%rdx), %xmm11\n  movdqu 176(%rdx), %xmm12\n  movdqu 192(%rdx), %xmm13\n  movdqu 208(%rdx), %xmm14\n  movdqu 224(%rdx), %xmm15\n"
    "  movq 64(%rdx), %rsp\n  movl $1, %eax\n  jmpq *72(%rdx)\n");
#define FRAME_WORDS 30          /* 168 bytes of xmm (21 words) + 8 registers + return address */
#define FRAME_R12 (21 + 3)
#define FRAME_RBX (21 + 6)
#else
__asm__(
    ".text\n.globl rt_fsw\nrt_fsw:\n"
    "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  movq %rsp, (%rdi)\n  movq %rsi, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n  ret\n"
    ".globl rt_fboot\nrt_fboot:\n  movq %rbx, %rdi\n  callq *%r12\n  ud2\n"
    ".globl rt_ssave\nrt_ssave:\n"
    "  movq %rbx, 0(%rdi)\n  movq %rbp, 8(%rdi)\n  movq %r12, 16(%rdi)\n  movq %r13, 24(%rdi)\n  movq %r14, 32(%rdi)\n  movq %r15, 40(%rdi)\n"
    "  leaq 8(%rsp), %rdx\n  movq %rdx, 48(%rdi)\n  movq (%rsp), %rdx\n  movq %rdx, 56(%rdi)\n"
    "  xorl %eax, %eax\n  ret\n"
    ".globl rt_sjump\nrt_sjump:\n"
    "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  movq %rsp, (%rdi)\n"
    "  movq 0(%rsi), %rbx\n  movq 8(%rsi), %rbp\n  movq 16(%rsi), %r12\n  movq 24(%rsi), %r13\n  movq 32(%rsi), %r14\n  movq 40(%rsi), %r15\n"
    "  movq 48(%rsi), %rsp\n  movl $1, %eax\n  jmpq *56(%rsi)\n");
#define FRAME_WORDS 7
#define FRAME_R12 3
#define FRAME_RBX 4
#endif

#ifdef _WIN32
/* Windows checks that the stack pointer is inside the thread's stack (TEB StackBase /
   StackLimit / DeallocationStack) when it handles an exception: keep them on the running fiber */
static void tib_set(rt_fiber *f) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    tib->StackBase = f->hi; tib->StackLimit = f->lo;
    *(void **)((uint8_t *)NtCurrentTeb() + 0x1478) = f->lo;
}
#else
static void tib_set(rt_fiber *f) { (void)f; }
#endif

static uint8_t *arena(void) { return (uint8_t *)(uintptr_t)RT_ARENA_VA; }
static int write_watch_ok RT_LOCAL;               /* (Windows: guest memory and the arena report written pages - rollback) */
static void *map_fixed(uint64_t va, uint64_t size, int commit) {
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)va, size, MEM_RESERVE | MEM_WRITE_WATCH | (commit ? MEM_COMMIT : 0), PAGE_READWRITE);
    if (p == (void *)(uintptr_t)va) { write_watch_ok++; return p; }
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    p = VirtualAlloc((void *)(uintptr_t)va, size, MEM_RESERVE | (commit ? MEM_COMMIT : 0), PAGE_READWRITE);
    return p == (void *)(uintptr_t)va ? p : NULL;
#else
    (void)commit;
    void *p = mmap((void *)(uintptr_t)va, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (p != (void *)(uintptr_t)va) { munmap(p, size); return NULL; }
    return p;
#endif
}
static int commit(void *p, uint64_t size) {
#ifdef _WIN32
    return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
    (void)p; (void)size; return 1;
#endif
}

/* guest memory + the arena (ARAM, fiber stacks), at their fixed addresses. Falls back to
   anywhere (no save states) if those addresses are taken. */
uint8_t *rt_layout_init(void) {
    uint8_t *g = map_fixed(RT_GUEST_VA, 0x40000000ull, 1);
    uint64_t arena_size = STACKS_OFF + MAXFIB * STACK_STRIDE;
    if (g && map_fixed(RT_ARENA_VA, arena_size, 0) && commit(arena(), ARAM_SIZE)) layout_ok = 1;
    if (!g) {
        rt_log("state", "guest memory can't go at its fixed address: save states and online play are off");
#ifdef _WIN32
        g = VirtualAlloc(NULL, 0x40000000ull, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        g = mmap(NULL, 0x40000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (g == MAP_FAILED) g = NULL;
#endif
    }
    if (!layout_ok) rt_log("state", "no fixed arena: save states and online play are off");
    return g;
}
int rt_state_ok(void) { return layout_ok; }
static uint8_t *aram_fallback RT_LOCAL;
uint8_t *rt_aram(void) {
    if (layout_ok) return arena();
    if (!aram_fallback) aram_fallback = calloc(1, ARAM_SIZE);
    return aram_fallback;
}

rt_fiber *plat_fiber_main(void) {                  /* the process's own stack (main) */
#ifdef _WIN32
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    host_fiber.hi = tib->StackBase; host_fiber.lo = *(uint8_t **)((uint8_t *)NtCurrentTeb() + 0x1478);
#endif
    host_fiber.slot = -1;
    if (!fib_cur) fib_cur = &host_fiber;
    return &host_fiber;
}
static void fiber_entry(rt_fiber *f) { f->fn(f->arg); rt_fatal("fiber returned"); }
rt_fiber *plat_fiber_create(void (*fn)(void *), void *arg) {
    if (nfib >= MAXFIB) rt_fatal("too many guest threads");
    rt_fiber *f = &fibers[nfib]; f->slot = nfib++;
    uint64_t size = f->slot == 0 ? MAIN_STACK : THREAD_STACK;
    uint8_t *base;
    if (layout_ok) {
        base = arena() + STACKS_OFF + (uint64_t)f->slot * STACK_STRIDE;
        if (!commit(base, size)) rt_fatal("can't commit a guest thread stack");
    } else {
#ifdef _WIN32
        base = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#endif
        if (!base) rt_fatal("can't allocate a guest thread stack");
    }
    f->lo = base; f->hi = base + size; f->fn = fn; f->arg = arg;
    uint64_t *end = (uint64_t *)((uintptr_t)(f->hi - 64) & ~(uintptr_t)15);
    uint64_t *sp = end - FRAME_WORDS;
    memset(sp, 0, FRAME_WORDS * 8);
    sp[FRAME_WORDS - 1] = (uint64_t)(uintptr_t)rt_fboot;
    sp[FRAME_R12] = (uint64_t)(uintptr_t)fiber_entry;
    sp[FRAME_RBX] = (uint64_t)(uintptr_t)f;
    f->sp = sp;
    return f;
}
void plat_fiber_switch(rt_fiber *from, rt_fiber *to) {
    fib_cur = to;
    tib_set(to);
    rt_fsw(&from->sp, to->sp);
}

/* ---- save states ---- */
#ifdef _WIN32
static void section_range(const char *name, uint8_t **lo, uint64_t *n) {
    uint8_t *img = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    *lo = NULL; *n = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!strncmp((const char *)sh[i].Name, name, 8)) { *lo = img + sh[i].VirtualAddress; *n = sh[i].Misc.VirtualSize; }
}
#else
extern char __start_rts_d[] __attribute__((weak)), __stop_rts_d[] __attribute__((weak));
extern char __start_rts_b[] __attribute__((weak)), __stop_rts_b[] __attribute__((weak));
#endif

/* the regions of a state: id, address, size (the stacks: from their saved stack pointer up) */
enum { RG_MEM1, RG_MEM2, RG_LC, RG_ARAM, RG_DATA, RG_BSS, RG_STACK0 };
#define SNAP_MAGIC 0x31534E52u      /* "RNS1" */
typedef struct { uint64_t addr, size; } region;
typedef struct {
    uint32_t magic, build, nreg, cur_slot;
    uint64_t total;
    region reg[RG_STACK0 + MAXFIB];
} snap_hdr;
struct rt_snap { uint8_t *buf; size_t cap, len; };
static rt_jb snap_jb RT_STATE;              /* where the state was taken (in the state: a load resumes there) */

static uint32_t build_id(void) {            /* states only load into the same build */
    uint32_t h = 2166136261u; const char *s = __DATE__ " " __TIME__;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    h ^= (uint32_t)(uintptr_t)&rt_fsw;      /* (and the same code addresses) */
    return h;
}
static int regions(region *r, uint8_t *cur_sp) {
    int n = 0;
    r[n++] = (region){(uint64_t)(uintptr_t)g_base, 33u << 20};   /* (MEM1 + the PC mod above it, 0x81800000-, + the native GX registers and IOS requests at 0x82000000) */
    r[n++] = (region){(uint64_t)(uintptr_t)(g_base + 0x10000000u), 64u << 20};
    r[n++] = (region){(uint64_t)(uintptr_t)(g_base + 0x20000000u), 16u << 10};
    r[n++] = (region){RT_ARENA_VA, ARAM_SIZE};
    uint8_t *lo; uint64_t sz;
#ifdef _WIN32
    section_range("rtd", &lo, &sz); r[n++] = (region){(uint64_t)(uintptr_t)lo, sz};
    section_range("rtb", &lo, &sz); r[n++] = (region){(uint64_t)(uintptr_t)lo, sz};
#else
    lo = (uint8_t *)__start_rts_d; sz = (uint64_t)(__stop_rts_d - __start_rts_d); r[n++] = (region){(uint64_t)(uintptr_t)lo, sz};
    lo = (uint8_t *)__start_rts_b; sz = (uint64_t)(__stop_rts_b - __start_rts_b); r[n++] = (region){(uint64_t)(uintptr_t)lo, sz};
#endif
    for (int i = 0; i < nfib; i++) {
        rt_fiber *f = &fibers[i];
        uint8_t *sp = f == fib_cur ? cur_sp : (uint8_t *)f->sp;
        sp = (uint8_t *)((uintptr_t)sp & ~(uintptr_t)15) - 256;        /* (a little below: red zone) */
        if (sp < f->lo) sp = f->lo;
        r[n++] = (region){(uint64_t)(uintptr_t)sp, (uint64_t)(f->hi - sp)};
    }
    return n;
}
rt_snap *rt_snap_new(void) { return calloc(1, sizeof(rt_snap)); }
void rt_snap_free(rt_snap *s) { if (s) { free(s->buf); free(s); } }
const uint8_t *rt_snap_data(rt_snap *s, size_t *n) { *n = s->len; return s->buf; }
int rt_snap_set(rt_snap *s, const uint8_t *p, size_t n) {
    if (n < sizeof(snap_hdr)) return 0;
    const snap_hdr *h = (const snap_hdr *)p;
    if (h->magic != SNAP_MAGIC || h->build != build_id() || h->total != n) return 0;
    if (s->cap < n) { free(s->buf); s->buf = malloc(n); s->cap = n; }
    memcpy(s->buf, p, n); s->len = n;
    return 1;
}

/* Take a state of the running game. 0: taken; 1: a load of it resumed here. Called on a guest
   thread (rt_det.c: at a video field). The copy runs inside this function, so its frame (and
   everything above it on this stack) is in the state as it is now. */
__attribute__((noinline)) int rt_snap_take(rt_snap *s) {
    if (!layout_ok || fib_cur == &host_fiber) return -1;
    if (rt_ssave(&snap_jb)) return 1;
    snap_hdr h; memset(&h, 0, sizeof h);
    h.magic = SNAP_MAGIC; h.build = build_id(); h.cur_slot = (uint32_t)fib_cur->slot;
    uint8_t *sp = (uint8_t *)(uintptr_t)snap_jb.q[
#ifdef _WIN32
        8
#else
        6
#endif
    ];
    h.nreg = (uint32_t)regions(h.reg, sp);
    uint64_t total = sizeof h; for (uint32_t i = 0; i < h.nreg; i++) total += h.reg[i].size;
    h.total = total;
    if (s->cap < total) { free(s->buf); s->buf = malloc(total); s->cap = total; if (!s->buf) { s->cap = 0; return -1; } }
    uint8_t *o = s->buf + sizeof h;
    for (uint32_t i = 0; i < h.nreg; i++) { memcpy(o, (void *)(uintptr_t)h.reg[i].addr, h.reg[i].size); o += h.reg[i].size; }
    memcpy(s->buf, &h, sizeof h);
    s->len = total;
    return 0;
}

/* Loading: on the host fiber (main's stack), which nothing in a state touches */
static rt_snap *pending RT_LOCAL;
static void apply(rt_snap *s) {
    const snap_hdr *h = (const snap_hdr *)s->buf;
    const uint8_t *in = s->buf + sizeof *h;
    for (uint32_t i = 0; i < h->nreg; i++) {
        uint8_t *dst = (uint8_t *)(uintptr_t)h->reg[i].addr;
        if (i >= RG_STACK0) {                       /* (a thread this process hasn't made yet: its stack) */
            uintptr_t lo = (uintptr_t)dst & ~(uintptr_t)0xFFFF, hi = ((uintptr_t)dst + h->reg[i].size + 0xFFFF) & ~(uintptr_t)0xFFFF;
            commit((void *)lo, hi - lo);
        }
        memcpy(dst, in, h->reg[i].size); in += h->reg[i].size;
    }
}
void ios_state_loaded(void), gx_render_state_loaded(void), remotes_state_loaded(void), det_pace_reset(void);
static int rb_pending RT_LOCAL; static uint32_t rb_target RT_LOCAL;
static void rb_apply(uint32_t field);
static rt_jb rb_jb RT_STATE;                        /* (where the rollback state of a field was taken) */
/* main(), after starting the guest: the host fiber waits here for loads */
void rt_state_host_loop(rt_fiber *guest_main) {
    plat_fiber_switch(&host_fiber, guest_main);
    for (;;) {                                      /* (back here: a load) */
        if (rb_pending) {                           /* a rollback: back to the state of field rb_target */
            rb_pending = 0;
            rb_apply(rb_target);
            ios_state_loaded(); gx_render_state_loaded();
            tib_set(fib_cur);
            rt_sjump(&host_fiber.sp, &rb_jb);       /* -> rb_save returns 1 */
            continue;                               /* (the next switch to the host fiber lands here) */
        }
        rt_snap *s = pending; pending = NULL;
        apply(s);
        ios_state_loaded(); gx_render_state_loaded(); remotes_state_loaded(); det_pace_reset();
        rt_fiber *f = fib_cur;                      /* (as it was when the state was taken) */
        tib_set(f);
        rt_sjump(&host_fiber.sp, &snap_jb);         /* -> rt_snap_take returns 1 */
    }
}
void rt_snap_load(rt_snap *s) {
    if (!layout_ok || !s || !s->len) return;
    pending = s;
    rt_fiber *from = fib_cur;
    fib_cur = &host_fiber;
    tib_set(&host_fiber);
    rt_fsw(&from->sp, host_fiber.sp);
    rt_fatal("rt_snap_load returned");
}

/* testing: where the live state differs from a taken one (logs the first few spots per region) */
void rt_snap_compare(rt_snap *s) {
    const snap_hdr *h = (const snap_hdr *)s->buf;
    const uint8_t *in = s->buf + sizeof *h;
    for (uint32_t i = 0; i < h->nreg; i++) {
        const uint8_t *live = (const uint8_t *)(uintptr_t)h->reg[i].addr; int shown = 0; uint64_t ndiff = 0;
        for (uint64_t o = 0; o < h->reg[i].size; o++) if (live[o] != in[o]) {
            ndiff++;
            if (shown < 6) { shown++; rt_log("state", "region %u (%llX + %llX): live %02X state %02X", i, (unsigned long long)h->reg[i].addr, (unsigned long long)o, live[o], in[o]); }
        }
        if (ndiff) rt_log("state", "region %u: %llu bytes differ", i, (unsigned long long)ndiff);
        in += h->reg[i].size;
    }
}

/* testing (RT_DIRTY_PROBE=1, rt_det.c): how many 4 KB pages of a state change per field, by
   region - what a rollback state each field would have to copy */
#define XXH_INLINE_ALL
#include "xxhash.h"
void rt_dirty_probe(void) {
    static uint64_t *prev RT_LOCAL; static uint32_t nprev RT_LOCAL; static uint64_t sum[RG_STACK0 + MAXFIB] RT_LOCAL, nf RT_LOCAL, tns RT_LOCAL;
    region r[RG_STACK0 + MAXFIB];
    uint8_t probe; int n = regions(r, &probe);
    uint32_t total = 0; for (int i = 0; i < n; i++) total += (uint32_t)((r[i].size + 4095) / 4096);
    uint64_t *cur = malloc(total * 8); uint32_t k = 0; uint64_t t = rt_now_ns();
    uint32_t dirty[RG_STACK0 + MAXFIB] = {0};
    for (int i = 0; i < n; i++) for (uint64_t o = 0; o < r[i].size; o += 4096, k++) {
        uint64_t len = r[i].size - o < 4096 ? r[i].size - o : 4096;
        cur[k] = XXH3_64bits((const void *)(uintptr_t)(r[i].addr + o), len);
        if (prev && nprev == total && prev[k] != cur[k]) dirty[i]++;
    }
    tns += rt_now_ns() - t;
    if (prev && nprev == total) { for (int i = 0; i < n; i++) sum[i] += dirty[i]; nf++; }
    free(prev); prev = cur; nprev = total;
    if (nf && nf % 120 == 0) {
        char b[512]; int m = 0; uint64_t all = 0;
        static const char *nm[] = {"MEM1", "MEM2", "LC", "ARAM", "data", "bss"};
        for (int i = 0; i < n && m < 480; i++) { all += sum[i]; if (sum[i]) m += snprintf(b + m, sizeof b - m, " %s %.0f", i < RG_STACK0 ? nm[i] : "stack", (double)sum[i] / nf); }
        rt_log("dirty", "pages changed per field (avg of %llu): %.0f = %.2f MB:%s (hashing %.1f ms/field)", (unsigned long long)nf, (double)all / nf, all * 4096.0 / nf / 1048576.0, b, tns / 1e6 / nf);
        memset(sum, 0, sizeof sum); nf = 0; tns = 0;
    }
}


/* ======================================================================== rollback states
 * Online play (rt_net.c) keeps a state of every video field of the last RB_N, so that when a
 * player's buttons arrive that differ from the ones guessed, the game goes back to that field
 * and runs the fields since again. A full state is ~117 MB; a field changes ~150 pages of it.
 * So: a shadow copy of the state as it was at the last field, and per field an undo list - the
 * pages that changed during it, with their contents before:
 *   - guest memory (MEM1, MEM2) and ARAM: the OS says which pages were written (Windows: write
 *     watch; Linux: the pages are read-only until written, a fault makes one writable and notes
 *     it);
 *   - the locked cache and the runtime's own state (the RT_STATE sections): compared with the
 *     shadow, page by page;
 *   - the guest threads' stacks: the used part, copied whole each field.
 * Going back to field R: the undo lists of the fields since R are applied newest first (on the
 * host fiber, like loading a state), the stacks of R copied back, and execution jumps to where
 * rb_save took R's state. */
#define RB_N 48
#define RB_REG 8
typedef struct { uint8_t *base; uint64_t size; int watched; uint8_t *shadow; uint32_t npages; uint8_t *mark; uintptr_t *ww; } rbreg_t;
static rbreg_t RB[RB_REG] RT_LOCAL; static int nrb RT_LOCAL;
typedef struct { uint8_t *at; uint64_t len; size_t off; } rbstack_t;
typedef struct {
    uint32_t field; int used;
    uint32_t n, cap; uint32_t *pg; uint8_t *old;     /* undo: region << 24 | page, and the page as it was at `field` */
    int nst; rbstack_t st[MAXFIB]; uint8_t *stk; size_t stk_len, stk_cap;
} rbrec_t;
static rbrec_t rbr[RB_N] RT_LOCAL;
static int rb_on RT_LOCAL, rb_have RT_LOCAL; static uint32_t rb_last RT_LOCAL;
static uint64_t rb_stat_skips RT_LOCAL, rb_stat_pages RT_LOCAL, rb_stat_saves RT_LOCAL, rb_stat_ns RT_LOCAL;

/* ---- which pages were written (dirty tracking) ---- */
#ifdef _WIN32
static int dt_start(void) {
    if (write_watch_ok < 2) return 0;              /* (guest memory and the arena both watched) */
    for (int r = 0; r < nrb; r++) if (RB[r].watched) {
        if (!RB[r].ww) RB[r].ww = malloc(sizeof(uintptr_t) * RB[r].npages);
        if (ResetWriteWatch(RB[r].base, RB[r].size)) return 0;
    }
    return 1;
}
static void dt_stop(void) {}
static void dt_reset(void) { for (int r = 0; r < nrb; r++) if (RB[r].watched) ResetWriteWatch(RB[r].base, RB[r].size); }
static void dt_collect(void (*fn)(int r, uint32_t page)) {
    for (int r = 0; r < nrb; r++) if (RB[r].watched) {
        ULONG_PTR n = RB[r].npages; ULONG gran = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, RB[r].base, RB[r].size, (PVOID *)RB[r].ww, &n, &gran) != 0) { rt_log("state", "GetWriteWatch failed"); continue; }
        for (ULONG_PTR i = 0; i < n; i++) fn(r, (uint32_t)(((uint8_t *)RB[r].ww[i] - RB[r].base) >> 12));
    }
}
#else
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
static struct sigaction dt_old RT_LOCAL;
static uint32_t *dt_list RT_LOCAL; static volatile uint32_t dt_n RT_LOCAL; static uint32_t dt_cap RT_LOCAL;
static volatile int dt_armed RT_LOCAL;
static void dt_fault(int sig, siginfo_t *si, void *uc) {
    uint8_t *a = (uint8_t *)si->si_addr;
    if (dt_armed) for (int r = 0; r < nrb; r++) if (RB[r].watched && a >= RB[r].base && a < RB[r].base + RB[r].size) {
        uint32_t pg = (uint32_t)((a - RB[r].base) >> 12);
        if (!RB[r].mark[pg]) { RB[r].mark[pg] = 1; if (dt_n < dt_cap) dt_list[dt_n++] = (uint32_t)r << 24 | pg; }
        mprotect(RB[r].base + ((uint64_t)pg << 12), 4096, PROT_READ | PROT_WRITE);
        return;
    }
    /* not ours: the crash handler */
    {   extern void *g_poll_site;
        char m[160]; int n = snprintf(m, sizeof m, "*** fault at %p (rip %p)\n", si->si_addr, (void *)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP]);
        if (write(2, m, (size_t)n) < 0) {} }
    if (dt_old.sa_flags & SA_SIGINFO) dt_old.sa_sigaction(sig, si, uc);
    else if (dt_old.sa_handler != SIG_DFL && dt_old.sa_handler != SIG_IGN) dt_old.sa_handler(sig);
    else { signal(sig, SIG_DFL); raise(sig); }
}
static int dt_start(void) {
    uint32_t total = 0;
    for (int r = 0; r < nrb; r++) if (RB[r].watched) { total += RB[r].npages; if (!RB[r].mark) RB[r].mark = calloc(1, RB[r].npages); else memset(RB[r].mark, 0, RB[r].npages); }
    if (!dt_list) { dt_cap = total; dt_list = malloc(sizeof(uint32_t) * total); }
    dt_n = 0;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = dt_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
    struct sigaction prev; sigaction(SIGSEGV, &sa, &prev);
    if (prev.sa_sigaction != dt_fault) dt_old = prev;
    dt_armed = 1;
    for (int r = 0; r < nrb; r++) if (RB[r].watched && mprotect(RB[r].base, RB[r].size, PROT_READ)) { rt_log("state", "mprotect failed (%d)", errno); return 0; }
    return 1;
}
static void dt_stop(void) {
    dt_armed = 0;
    for (int r = 0; r < nrb; r++) if (RB[r].watched) mprotect(RB[r].base, RB[r].size, PROT_READ | PROT_WRITE);
}
static void dt_reprotect(void) {
    for (uint32_t i = 0; i < dt_n; i++) { int r = (int)(dt_list[i] >> 24); uint32_t pg = dt_list[i] & 0xFFFFFF;
        RB[r].mark[pg] = 0; mprotect(RB[r].base + ((uint64_t)pg << 12), 4096, PROT_READ); }
    dt_n = 0;
}
static void dt_reset(void) { dt_reprotect(); }
static void dt_collect(void (*fn)(int r, uint32_t page)) {
    for (uint32_t i = 0; i < dt_n; i++) fn((int)(dt_list[i] >> 24), dt_list[i] & 0xFFFFFF);
    dt_reprotect();
}
#endif

/* a file read into guest memory, through a buffer: on Linux the kernel can't write a read-only
   (watched) page; on Windows a write by the kernel might not count as a written page */
size_t rt_fread_guest(void *dst, size_t n, FILE *f) {
    static uint8_t buf[1 << 16]; size_t done = 0;
    while (done < n) {
        size_t want = n - done < sizeof buf ? n - done : sizeof buf, got = fread(buf, 1, want, f);
        memcpy((uint8_t *)dst + done, buf, got); done += got;
        if (got < want) break;
    }
    return done;
}

/* ---- undo lists ---- */
static rbrec_t *rb_rec(uint32_t field) { rbrec_t *r = &rbr[field % RB_N]; return r->used && r->field == field ? r : NULL; }
static void rb_note(int r, uint32_t page) {          /* page `page` of region r changed since the last state */
    uint8_t *cur = RB[r].base + ((uint64_t)page << 12), *sh = RB[r].shadow + ((uint64_t)page << 12);
    uint64_t len = RB[r].size - ((uint64_t)page << 12); if (len > 4096) len = 4096;
    if (rb_have) {
        rbrec_t *rec = &rbr[rb_last % RB_N];
        if (rec->n == rec->cap) { rec->cap = rec->cap ? rec->cap * 2 : 512; rec->pg = realloc(rec->pg, rec->cap * 4); rec->old = realloc(rec->old, (size_t)rec->cap * 4096); }
        rec->pg[rec->n] = (uint32_t)r << 24 | page; memcpy(rec->old + (size_t)rec->n * 4096, sh, len); rec->n++;
    }
    memcpy(sh, cur, len);
}
static void rb_collect(void) {
    static int dbg = -1; if (dbg < 0) dbg = getenv("RT_RB_DEBUG") != NULL;
    uint64_t t0 = rt_now_ns();
    dt_collect(rb_note);
    uint64_t t1 = rt_now_ns();
    for (int r = 0; r < nrb; r++) if (!RB[r].watched)              /* (compared) */
        for (uint32_t p = 0; p < RB[r].npages; p++) {
            uint64_t o = (uint64_t)p << 12, len = RB[r].size - o < 4096 ? RB[r].size - o : 4096;
            if (memcmp(RB[r].base + o, RB[r].shadow + o, len)) rb_note(r, p);
        }
    if (dbg) { static uint64_t n, a, b; n++; a += t1 - t0; b += rt_now_ns() - t1;
        if (n % 60 == 0) { uint64_t cmp = 0; for (int r = 0; r < nrb; r++) if (!RB[r].watched) cmp += RB[r].size;
            rt_log("state", "collect: watched %.2f ms, compared %.2f ms (%.1f MB) (avg)", a / 1e6 / 60, b / 1e6 / 60, cmp / 1048576.0); a = b = 0; } }
}
static void rb_reg_add(uint8_t *base, uint64_t size, int watched) {
    rbreg_t *g = &RB[nrb++]; g->base = base; g->size = size; g->watched = watched; g->npages = (uint32_t)((size + 4095) >> 12);
    g->shadow = malloc((size_t)g->npages << 12); memcpy(g->shadow, base, size);
}
/* start keeping states (the online game began, or a state was loaded): 1 = on */
int rb_start(void) {
    if (!layout_ok) return 0;
    rb_stop();
    if (!nrb) {
        rb_reg_add(g_base, 33u << 20, 1);                                  /* MEM1 (+ the PC mod above it, + 0x82000000-0x82100000) */
        rb_reg_add(g_base + 0x10000000u, 64u << 20, 1);                    /* MEM2 */
        rb_reg_add(arena(), ARAM_SIZE, 1);                                 /* ARAM */
        rb_reg_add(g_base + 0x20000000u, 16u << 10, 0);                    /* locked cache */
        uint8_t *lo; uint64_t sz;
#ifdef _WIN32
        section_range("rtd", &lo, &sz); rb_reg_add(lo, sz, 0);
        section_range("rtb", &lo, &sz); rb_reg_add(lo, sz, 0);
#else
        lo = (uint8_t *)__start_rts_d; sz = (uint64_t)(__stop_rts_d - __start_rts_d); rb_reg_add(lo, sz, 0);
        lo = (uint8_t *)__start_rts_b; sz = (uint64_t)(__stop_rts_b - __start_rts_b); rb_reg_add(lo, sz, 0);
#endif
    } else for (int r = 0; r < nrb; r++) memcpy(RB[r].shadow, RB[r].base, RB[r].size);
    for (int i = 0; i < RB_N; i++) { rbr[i].used = 0; rbr[i].n = 0; }
    rb_have = 0;
    if (!dt_start()) { dt_stop(); rt_log("state", "rollback: no way to see written pages here - off"); return 0; }
    rb_on = 1;
    rt_log("state", "rollback states on");
    return 1;
}
void rb_stop(void) { if (rb_on) { rb_on = 0; dt_stop(); } }
int rb_active(void) { return rb_on; }
int rb_supported(void) {
#ifdef _WIN32
    return layout_ok && write_watch_ok >= 2;
#else
    return layout_ok;
#endif
}
/* the state of field `field`, at the start of it (det_field). 0: kept; 1: a rollback to it resumed
   here; -1: off. keep 0: a field whose buttons are final isn't kept (no rollback can go to it):
   what it changes goes into the undo list of the newest kept state, which stays exact */
__attribute__((noinline)) int rb_save(uint32_t field, int keep) {
    if (!rb_on || fib_cur == &host_fiber) return -1;
    if (!keep) { rb_stat_skips++; return 0; }      /* (final: nothing can go back to it - its changes go to the state before) */
    if (rt_ssave(&rb_jb)) return 1;
    uint64_t t = rt_now_ns();
    rb_collect();
    rbrec_t *rec = &rbr[field % RB_N];
    rec->field = field; rec->used = 1; rec->n = 0; rec->nst = 0; rec->stk_len = 0;
    uint8_t *cur_sp = (uint8_t *)(uintptr_t)rb_jb.q[
#ifdef _WIN32
        8
#else
        6
#endif
    ];
    for (int i = 0; i < nfib; i++) {                  /* the stacks, from each one's stack pointer up */
        rt_fiber *f = &fibers[i];
        uint8_t *sp = f == fib_cur ? cur_sp : (uint8_t *)f->sp;
        sp = (uint8_t *)((uintptr_t)sp & ~(uintptr_t)15) - 256;
        if (sp < f->lo) sp = f->lo;
        uint64_t len = (uint64_t)(f->hi - sp);
        if (rec->stk_len + len > rec->stk_cap) { rec->stk_cap = (rec->stk_len + len) * 2; rec->stk = realloc(rec->stk, rec->stk_cap); }
        memcpy(rec->stk + rec->stk_len, sp, len);
        rec->st[rec->nst++] = (rbstack_t){sp, len, rec->stk_len}; rec->stk_len += len;
    }
    rb_last = field; rb_have = 1;
    rb_stat_ns += rt_now_ns() - t; rb_stat_saves++;
    {   static int dbg = -1; if (dbg < 0) dbg = getenv("RT_RB_DEBUG") != NULL;
        if (dbg && rb_stat_saves % 60 == 1) { uint32_t nd = 0; (void)nd;
            rt_log("state", "rb_save %u: %d stacks, %.1f KB of stack, %.2f ms", field, rec->nst, rec->stk_len / 1024.0, (rt_now_ns() - t) / 1e6); } }
    return 0;
}
int rb_can_go_back(uint32_t field) { return rb_on && rb_have && (int32_t)(rb_last - field) >= 0 && rb_rec(field) != NULL; }
uint32_t rb_newest(void) { return rb_last; }
/* go back to the state of field `field` (doesn't return: rb_save of that field returns 1) */
void rb_rollback(uint32_t field) {
    if (!rb_can_go_back(field)) rt_fatal("rollback to field %u: not kept", field);
    rb_pending = 1; rb_target = field;
    rt_fiber *from = fib_cur;
    fib_cur = &host_fiber;
    tib_set(&host_fiber);
    rt_fsw(&from->sp, host_fiber.sp);
    rt_fatal("rb_rollback returned");
}
static void rb_apply(uint32_t field) {
    uint64_t t = rt_now_ns(); uint64_t pages = 0;
    rb_collect();                                   /* (what changed since the newest state: into its undo list) */
    for (uint32_t f = rb_last; ; f--) {
        rbrec_t *rec = rb_rec(f);
        if (!rec) rt_fatal("rollback: the state of field %u is gone", f);
        for (uint32_t i = rec->n; i-- > 0; ) {
            int r = (int)(rec->pg[i] >> 24); uint32_t pg = rec->pg[i] & 0xFFFFFF;
            uint64_t o = (uint64_t)pg << 12, len = RB[r].size - o < 4096 ? RB[r].size - o : 4096;
            memcpy(RB[r].base + o, rec->old + (size_t)i * 4096, len);
            memcpy(RB[r].shadow + o, rec->old + (size_t)i * 4096, len);
        }
        pages += rec->n;
        if (f != field) rec->used = 0;
        if (f == field) break;
    }
    rbrec_t *rec = rb_rec(field);
    for (int i = 0; i < rec->nst; i++) memcpy(rec->st[i].at, rec->stk + rec->st[i].off, rec->st[i].len);
    rec->n = 0;                                      /* (its undo list fills again at the next field) */
    rb_last = field;
    dt_reset();                                      /* (what we just wrote isn't a change) */
    rb_stat_pages += pages;
    static uint64_t nlog; if (nlog++ < 20 || nlog % 100 == 0) rt_log("state", "rollback to field %u: %llu pages (%.2f ms)", field, (unsigned long long)pages, (rt_now_ns() - t) / 1e6);
}
/* (statistics for the session log) */
void rb_stats(double *save_ms, uint64_t *saves, uint64_t *skips) { *save_ms = rb_stat_saves ? rb_stat_ns / 1e6 / rb_stat_saves : 0; *saves = rb_stat_saves; if (skips) *skips = rb_stat_skips; rb_stat_ns = 0; rb_stat_saves = rb_stat_skips = 0; }
