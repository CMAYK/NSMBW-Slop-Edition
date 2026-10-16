/* rt_gx.c - GX command processor: parses the FIFO stream written to the write-gather pipe
 * (and display lists it calls), tracks CP / XF / BP state, raises PE token/finish
 * interrupts, and hands draw calls and EFB copies to the renderer (rt_gxrender.c). */
#include "rt.h"
#include "rt_gx.h"
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

gx_state gx;

/* ------------------------------------------------------------------ CP / PE registers */
static uint16_t cp_regs[0x80 / 2];
static uint16_t pe_regs[0x100 / 2];
static uint16_t pe_token;

/* The GX library no longer reaches these registers (its register pointers point at plain memory,
   rt_gx.c "the GX FIFO, native"); an access means something new does - logged once. */
static void hw_unexpected(const char *what, uint32_t reg) { static int n; if (n++ < 4) rt_log("gx", "unexpected %s register access %03X (track A1: should not happen)", what, reg); }
uint32_t gx_cp_read(uint32_t reg, int size) {
    reg &= 0x7F; hw_unexpected("CP", reg);
    switch (reg) {
    case 0x00: return 0x0C | 0x02;                         /* GP idle (cmd + read), underflow */
    case 0x30: case 0x32: return 0;                         /* read/write distance: drained */
    }
    if (size == 4) return ((uint32_t)cp_regs[reg / 2] << 16) | cp_regs[reg / 2 + 1];
    return cp_regs[reg / 2];
}
void gx_cp_write(uint32_t reg, uint32_t v, int size) {
    reg &= 0x7F; hw_unexpected("CP", reg);
    if (size == 4) { cp_regs[reg / 2] = v >> 16; cp_regs[reg / 2 + 1] = (uint16_t)v; }
    else cp_regs[reg / 2] = (uint16_t)v;
}
static void pe_update_irq(void) {                         /* (token / finish: always enabled - __GXPEInit enables both) */
    uint16_t ctl = pe_regs[0x0A / 2];
    pi_set(PI_PE_TOKEN, (ctl & 4) != 0);
    pi_set(PI_PE_FINISH, (ctl & 8) != 0);
}
uint32_t gx_pe_read(uint32_t reg, int size) {
    reg &= 0xFF; (void)size; hw_unexpected("PE", reg);
    if (reg == 0x0E) return pe_token;
    return pe_regs[reg / 2];
}
void gx_pe_write(uint32_t reg, uint32_t v, int size) {
    reg &= 0xFF; (void)size; hw_unexpected("PE", reg);
    if (reg == 0x0A) {                                      /* status bits are write-1-to-clear */
        uint16_t st = pe_regs[0x0A / 2] & 0x0C;
        st &= ~(v & 0x0C);
        pe_regs[0x0A / 2] = (uint16_t)((v & 0x03) | st);
        pe_update_irq();
        return;
    }
    pe_regs[reg / 2] = (uint16_t)v;
}

/* ------------------------------------------------------------------ vertex sizes */
static int comp_size(int fmt) { static const int s[8] = {1, 1, 2, 2, 4, 4, 4, 4}; return s[fmt & 7]; }
static int color_size(int fmt) { static const int s[8] = {2, 3, 4, 2, 3, 4, 4, 4}; return s[fmt & 7]; }
static int index_size(int type) { return type == 2 ? 1 : type == 3 ? 2 : 0; }

int gx_vertex_size(int vat) {
    uint32_t lo = gx.cp_vcd_lo, hi = gx.cp_vcd_hi;
    uint32_t a = gx.cp_vat_a[vat], b = gx.cp_vat_b[vat], c = gx.cp_vat_c[vat];
    int size = 0;
    if (lo & 1) size++;                                      /* pos/nrm matrix index */
    for (int i = 0; i < 8; i++) if (lo & (2u << i)) size++;  /* tex matrix indices */
    int pos = (lo >> 9) & 3, nrm = (lo >> 11) & 3, c0 = (lo >> 13) & 3, c1 = (lo >> 15) & 3;
    if (pos == 1) size += ((a & 1) ? 3 : 2) * comp_size((a >> 1) & 7); else size += index_size(pos);
    if (nrm) {
        int nbt = (a >> 9) & 1, n3 = (a >> 31) & 1, fmt = (a >> 10) & 7;
        int ncomp = nbt ? 9 : 3;
        if (nrm == 1) size += ncomp * comp_size(fmt);
        else size += (nbt && n3) ? 3 * index_size(nrm) : index_size(nrm);
    }
    if (c0 == 1) size += color_size((a >> 14) & 7); else size += index_size(c0);
    if (c1 == 1) size += color_size((a >> 18) & 7); else size += index_size(c1);
    for (int t = 0; t < 8; t++) {
        int type = (hi >> (2 * t)) & 3;
        if (!type) continue;
        int cnt, fmt;
        switch (t) {
        case 0: cnt = (a >> 21) & 1; fmt = (a >> 22) & 7; break;
        case 1: cnt = (b >> 0) & 1; fmt = (b >> 1) & 7; break;
        case 2: cnt = (b >> 9) & 1; fmt = (b >> 10) & 7; break;
        case 3: cnt = (b >> 18) & 1; fmt = (b >> 19) & 7; break;
        case 4: cnt = (b >> 27) & 1; fmt = (c >> 0) & 7; break;
        case 5: cnt = (c >> 5) & 1; fmt = (c >> 6) & 7; break;
        case 6: cnt = (c >> 14) & 1; fmt = (c >> 15) & 7; break;
        default: cnt = (c >> 23) & 1; fmt = (c >> 24) & 7; break;
        }
        if (type == 1) size += (cnt ? 2 : 1) * comp_size(fmt); else size += index_size(type);
    }
    return size;
}

/* ------------------------------------------------------------------ register loads */
static void load_cp(uint8_t reg, uint32_t v) {
    switch (reg & 0xF0) {
    case 0x30: gx.cp_matidx_a = v; return;
    case 0x40: gx.cp_matidx_b = v; return;
    case 0x50: gx.cp_vcd_lo = v; return;
    case 0x60: gx.cp_vcd_hi = v; return;
    case 0x70: gx.cp_vat_a[reg & 7] = v; return;
    case 0x80: gx.cp_vat_b[reg & 7] = v; return;
    case 0x90: gx.cp_vat_c[reg & 7] = v; return;
    case 0xA0: gx.cp_array_base[reg & 15] = v; return;
    case 0xB0: gx.cp_array_stride[reg & 15] = v & 0xFF; return;
    }
}
extern uint64_t g_frames_presented;
static int xf_trace(void) {
    static int init; static uint64_t at; static int left = 60;
    if (!init) { init = 1; const char *e = getenv("RT_XFTRACE"); at = e ? strtoull(e, NULL, 10) : 0; if (!e) left = 0; }
    return left > 0 && g_frames_presented >= at && left--;
}
static void load_xf(uint32_t addr, const uint8_t *p, int n) {
    if (addr != 0 && addr != 0x78 && addr < 0x1000 && xf_trace()) rt_log("xf", "load addr %04X n %d first %02X%02X%02X%02X", addr, n, p[0], p[1], p[2], p[3]);
    for (int i = 0; i < n; i++, addr++) {
        uint32_t v = ((uint32_t)p[4 * i] << 24) | (p[4 * i + 1] << 16) | (p[4 * i + 2] << 8) | p[4 * i + 3];
        if (addr < 0x800) gx.xf_mem[addr] = v;
        else if (addr >= 0x1000 && addr < 0x1100) gx.xf_regs[addr - 0x1000] = v;
    }
}
static void load_xf_indexed(int which, uint32_t v) {
    /* LOAD_INDX_A..D: (index<<16) | ((n-1)<<12) | addr ; arrays 12..15 */
    uint32_t idx = v >> 16, n = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    uint32_t src = gx.cp_array_base[12 + which] + idx * gx.cp_array_stride[12 + which];
    if (xf_trace()) rt_log("xf", "indexed %c idx %u n %u addr %03X src %08X (base %08X stride %u) first %08X", 'A' + which, idx, n, addr, src,
                           gx.cp_array_base[12 + which], gx.cp_array_stride[12 + which], R32(0x80000000u | src));
    for (uint32_t i = 0; i < n; i++) if (addr + i < 0x800) gx.xf_mem[addr + i] = R32(0x80000000u | (src + 4 * i));
}
uint32_t g_gx_state_gen;                                     /* bumped whenever render state may have changed */
static void load_bp(uint32_t v) {
    uint8_t reg = v >> 24; uint32_t val = v & 0xFFFFFF;
    if (reg == 0xFE) { gx.bp_mask = val; return; }
    uint32_t old = gx.bp[reg];
    gx.bp[reg] = (gx.bp[reg] & ~gx.bp_mask) | (val & gx.bp_mask);
    gx.bp_mask = 0xFFFFFF;
    val = gx.bp[reg];
    if (val != old || reg == 0x0F || reg == 0x52 || reg == 0x64 || reg == 0x65 || (reg >= 0xE0 && reg <= 0xE7)) g_gx_state_gen++;
    switch (reg) {
    case 0x45:                                               /* PE_DONE: draw done */
        pe_regs[0x0A / 2] |= 8; pe_update_irq(); gx_render_flush(); break;
    case 0x47:                                               /* token with interrupt */
        pe_token = val & 0xFFFF; pe_regs[0x0A / 2] |= 4; pe_update_irq(); break;
    case 0x48: pe_token = val & 0xFFFF; break;
    case 0x52: gx_render_efb_copy(val); break;               /* EFB copy execute */
    case 0x64: case 0x65: gx_render_tlut_load(reg, val); break;
    case 0x0F: gx_render_tex_invalidate(); break;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7: gx_render_tev_reg(reg, val); break;
    }
}

/* ------------------------------------------------------------------ command parser */
static uint8_t fifo[1 << 20]; static uint32_t fifo_len;
uint64_t g_gx_draws RT_LOCAL, g_gx_cmds RT_LOCAL;

/* parse commands from p[0..len); returns bytes consumed (stops at an incomplete command) */
static uint32_t parse(const uint8_t *p, uint32_t len, int depth) {
    uint32_t o = 0;
    while (o < len) {
        uint8_t cmd = p[o];
        uint32_t need;
        if (cmd == 0x00 || cmd == 0x48 || cmd == 0x44) { o++; continue; }
        if (cmd == 0x08) need = 6;
        else if (cmd == 0x10) { if (len - o < 5) break; need = 5 + 4 * ((((uint32_t)p[o + 1] << 8 | p[o + 2]) & 0xF) + 1); }
        else if ((cmd & 0xE7) == 0x20) need = 5;
        else if (cmd == 0x40) need = 9;
        else if (cmd == 0x61) need = 5;
        else if (cmd & 0x80) {
            if (len - o < 3) break;
            uint32_t cnt = ((uint32_t)p[o + 1] << 8) | p[o + 2];
            need = 3 + cnt * (uint32_t)gx_vertex_size(cmd & 7);
        } else {
            static int warned; if (warned++ < 8) rt_log("gx", "unknown FIFO opcode %02X (skipping byte)", cmd);
            o++; continue;
        }
        if (len - o < need) break;
        const uint8_t *q = p + o + 1;
        uint32_t w = ((uint32_t)q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3];
        g_gx_cmds++;
        static int gxtrace = -1; if (gxtrace < 0) gxtrace = getenv("RT_GX_TRACE") != NULL;
        if (gxtrace) rt_log("gxcmd", "%02X need=%u (%02X %02X %02X %02X %02X)", cmd, need, q[0], q[1], q[2], q[3], q[4]);
        if (cmd == 0x08) load_cp(q[0], ((uint32_t)q[1] << 24) | (q[2] << 16) | (q[3] << 8) | q[4]);
        else if (cmd == 0x10) load_xf(w & 0xFFFF, q + 4, (int)((w >> 16) & 0xF) + 1);
        else if ((cmd & 0xE7) == 0x20) load_xf_indexed((cmd >> 3) & 3, w);
        else if (cmd == 0x61) load_bp(w);
        else if (cmd == 0x40) {
            uint32_t addr = w & 0x3FFFFFE0, size = ((uint32_t)q[4] << 24) | (q[5] << 16) | (q[6] << 8) | q[7];
            if (depth < 4 && size) parse(g_base + addr, size, depth + 1);
        } else if (cmd & 0x80) {
            uint32_t cnt = ((uint32_t)p[o + 1] << 8) | p[o + 2];
            g_gx_draws++;
            gx_render_draw(cmd & 0x78, cmd & 7, cnt, p + o + 3);
        }
        o += need;
    }
    return o;
}

/* ------------------------------------------------------------------ the GX FIFO, native (track A1)
 * The GX library writes commands to the write-gather pipe; where they go depends on its FIFO
 * setup: the "CPU FIFO" is the buffer it writes, the "GP FIFO" the one the graphics chip reads.
 * When they are the same buffer ("linked", the normal case) the commands are executed at once
 * here; otherwise (a display list being recorded, GXBeginDisplayList) they are written into the
 * CPU FIFO's buffer in memory. The library's FIFO functions are partly native (below): its
 * hardware register pointers point at plain memory, and the few values it reads back (write
 * pointer, GP read pointer / count, GP status) come from here. docs/NATIVE_ROADMAP.md */
#define GX_R13            0x8042F680u
#define GX_DATA_PTR       (0x80433080u - 0x4EF8u)      /* __GXData (r2 - 0x4EF8) */
#define GX_CPU_SET        (GX_R13 - 0x4DC8u)          /* u8: a CPU FIFO is set */
#define GX_GP_SET         (GX_R13 - 0x4DC7u)          /* u8: a GP FIFO is set */
#define GX_LINKED         (GX_R13 - 0x4DB0u)          /* u8: CPU FIFO == GP FIFO */
#define GX_CPU_FIFO       0x80390224u                  /* the library's copies of the FIFO objects */
#define GX_GP_FIFO        0x80390200u
#define GX_REG_CP         (GX_R13 - 0x4DECu)          /* the library's register pointers */
#define GX_REG_PI         (GX_R13 - 0x4DF0u)
#define GX_REG_PE         (GX_R13 - 0x4DE8u)
#define GX_REG_MEM        (GX_R13 - 0x4DE4u)
#define GX_REG_SHADOW     0x82000000u                  /* where they point: plain memory (0x82000000 + the old offsets) */
enum { FO_BASE = 0, FO_TOP = 4, FO_SIZE = 8, FO_RDPTR = 0x14, FO_WRPTR = 0x18, FO_COUNT = 0x1C, FO_WRAP = 0x20 };
static uint32_t sink_base, sink_top, sink_wptr; static int sink_wrap, sink_on, sink_linked;

static FILE *fifo_dump RT_LOCAL; static int fifo_dump_init RT_LOCAL;
static void execute_pending(void) {
    uint32_t used = parse(fifo, fifo_len, 0);
    if (used) { memmove(fifo, fifo + used, fifo_len - used); fifo_len -= used; }
    if (fifo_len > sizeof fifo - 64) rt_fatal("GX FIFO parse stalled (%u bytes pending, first %02X)", fifo_len, fifo[0]);
}
void gx_fifo_write(uint32_t v, int size) {
    if (!fifo_dump_init) { fifo_dump_init = 1; const char *p = getenv("RT_FIFO_DUMP"); if (p) fifo_dump = fopen(p, "wb"); }
    if (fifo_dump) { for (int i = size - 1; i >= 0; i--) fputc((uint8_t)(v >> (8 * i)), fifo_dump); fflush(fifo_dump); }
    if (sink_linked) {                                       /* executed (batched: commands can span writes) */
        for (int i = size - 1; i >= 0; i--) fifo[fifo_len++] = (uint8_t)(v >> (8 * i));
        if (fifo_len >= 32 || fifo_len >= sizeof fifo - 8) execute_pending();
        return;
    }
    if (!sink_on) return;                                    /* (no CPU FIFO: the hardware drops it too) */
    for (int i = size - 1; i >= 0; i--) {                    /* recorded into the CPU FIFO's buffer */
        g_base[PPC_PHYS(sink_wptr)] = (uint8_t)(v >> (8 * i));
        if (++sink_wptr >= sink_top + 4) { sink_wptr = sink_base; sink_wrap = 1; }
    }
}

/* __GXFifoInit (GXInit calls it right after setting its register pointers): the pointers are
   moved to plain memory first, so the library never reaches the emulated registers again. */
void hle___GXFifoInit(ppc_context *ctx, uint8_t *base) {
    W32(GX_REG_CP, GX_REG_SHADOW); W32(GX_REG_PE, GX_REG_SHADOW + 0x1000);
    W32(GX_REG_PI, GX_REG_SHADOW + 0x3000); W32(GX_REG_MEM, GX_REG_SHADOW + 0x4000);
    orig___GXFifoInit(ctx, base);
    sink_on = sink_linked = 0;
}
/* GXSetCPUFifo(fifo): the library's bookkeeping, then the commands go to that buffer */
void hle_GXSetCPUFifo(ppc_context *ctx, uint8_t *base) {
    uint32_t f = ctx->r[3];
    if (sink_linked) execute_pending();
    orig_GXSetCPUFifo(ctx, base);
    if (!f) { sink_on = sink_linked = 0; return; }
    sink_base = R32(f + FO_BASE); sink_top = R32(f + FO_TOP); sink_wptr = R32(f + FO_WRPTR); sink_wrap = 0;
    sink_on = 1; sink_linked = R8(GX_LINKED) != 0;
}
void hle_GXSetGPFifo(ppc_context *ctx, uint8_t *base) {
    if (sink_linked) execute_pending();
    orig_GXSetGPFifo(ctx, base);
    sink_linked = sink_on && R8(GX_LINKED) != 0;
}
/* fn_801C32E0 (__GXSaveFifo): bring the FIFO objects' pointers up to date. Linked: everything
   written was executed (read pointer = write pointer, nothing pending). */
void hle_fn_801C32E0(ppc_context *ctx, uint8_t *base) {
    (void)ctx; (void)base;
    if (sink_linked) execute_pending();
    int cpu = R8(GX_CPU_SET) != 0, gp = R8(GX_GP_SET) != 0, linked = R8(GX_LINKED) != 0;
    if (cpu) { W32(GX_CPU_FIFO + FO_WRPTR, sink_wptr); W8(GX_CPU_FIFO + FO_WRAP, (uint8_t)sink_wrap); }
    if (gp && linked) { W32(GX_GP_FIFO + FO_RDPTR, sink_wptr); W32(GX_GP_FIFO + FO_COUNT, 0); }
    if (linked) {
        W32(GX_CPU_FIFO + FO_RDPTR, R32(GX_GP_FIFO + FO_RDPTR)); W32(GX_CPU_FIFO + FO_COUNT, R32(GX_GP_FIFO + FO_COUNT));
        W32(GX_GP_FIFO + FO_WRPTR, R32(GX_CPU_FIFO + FO_WRPTR)); W8(GX_GP_FIFO + FO_WRAP, R8(GX_CPU_FIFO + FO_WRAP));
    } else if (cpu) {
        int32_t n = (int32_t)(R32(GX_CPU_FIFO + FO_WRPTR) - R32(GX_CPU_FIFO + FO_RDPTR));
        if (n < 0) n += (int32_t)R32(GX_CPU_FIFO + FO_SIZE);
        W32(GX_CPU_FIFO + FO_COUNT, (uint32_t)n);
    }
}
/* GXGetGPStatus(&overHi, &underLo, &readIdle, &cmdIdle, &breakpoint): the commands are done */
void hle_GXGetGPStatus(ppc_context *ctx, uint8_t *base) {
    (void)base;
    if (sink_linked) execute_pending();
    W32(R32(GX_DATA_PTR) + 0xC, 0x0E);
    W8(ctx->r[3], 0); W8(ctx->r[4], 1); W8(ctx->r[5], 1); W8(ctx->r[6], 1); W8(ctx->r[7], 0);
}
/* the PE interrupt handlers acknowledge in the PE registers, which are plain memory now: the
   interrupt is taken back here before the library's handler runs */
void hle_GXTokenInterruptHandler(ppc_context *ctx, uint8_t *base) { pe_regs[0x0A / 2] &= ~4; pe_update_irq(); orig_GXTokenInterruptHandler(ctx, base); }
void hle_GXFinishInterruptHandler(ppc_context *ctx, uint8_t *base) { pe_regs[0x0A / 2] &= ~8; pe_update_irq(); orig_GXFinishInterruptHandler(ctx, base); }
/* GXAbortFrame (fn_801C4B70): reset the graphics pipe - here: drop what wasn't executed */
void hle_fn_801C4B70(ppc_context *ctx, uint8_t *base) { (void)ctx; (void)base; fifo_len = 0; }

void gx_init(void) {
    memset(&gx, 0, sizeof gx);
    gx.bp_mask = 0xFFFFFF;
    gx_render_init();
}

/* data cache flushes: textures in the flushed memory are re-checked in full (rt_gxrender.c) */
void gx_mark_dirty(uint32_t addr, uint32_t len);
void hle_DCFlushRange(ppc_context *ctx, uint8_t *base) { gx_mark_dirty(ctx->r[3], ctx->r[4]); orig_DCFlushRange(ctx, base); }
void hle_DCStoreRange(ppc_context *ctx, uint8_t *base) { gx_mark_dirty(ctx->r[3], ctx->r[4]); orig_DCStoreRange(ctx, base); }
void hle_DCFlushRangeNoSync(ppc_context *ctx, uint8_t *base) { gx_mark_dirty(ctx->r[3], ctx->r[4]); orig_DCFlushRangeNoSync(ctx, base); }
void hle_DCStoreRangeNoSync(ppc_context *ctx, uint8_t *base) { gx_mark_dirty(ctx->r[3], ctx->r[4]); orig_DCStoreRangeNoSync(ctx, base); }
