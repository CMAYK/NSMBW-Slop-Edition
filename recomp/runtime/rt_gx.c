/* rt_gx.c - GX command processor: parses the FIFO stream written to the write-gather pipe
 * (and display lists it calls), tracks CP / XF / BP state, raises PE token/finish
 * interrupts, and hands draw calls and EFB copies to the renderer (rt_gxrender.c). */
#include "rt.h"
#include "rt_gx.h"

gx_state gx;
extern void hw_pi_fifo_advance(uint32_t v, int size);
extern uint32_t hw_pi_fifo_wptr(void);
extern uint32_t hw_pi_fifo_base(void);

/* ------------------------------------------------------------------ CP / PE registers */
static uint16_t cp_regs[0x80 / 2];
static uint16_t pe_regs[0x100 / 2];
static uint16_t pe_token;

uint32_t gx_cp_read(uint32_t reg, int size) {
    reg &= 0x7F;
    switch (reg) {
    case 0x00: return 0x0C | 0x02;                         /* GP idle (cmd + read), underflow */
    case 0x30: case 0x32: return 0;                         /* read/write distance: drained */
    case 0x34: case 0x38: { uint32_t w = hw_pi_fifo_wptr() & 0x03FFFFE0; return reg == 0x34 || reg == 0x38 ? (w & 0xFFFF) : 0; }
    case 0x36: case 0x3A: return (hw_pi_fifo_wptr() >> 16) & 0x3FFF;
    }
    if (size == 4) return ((uint32_t)cp_regs[reg / 2] << 16) | cp_regs[reg / 2 + 1];
    return cp_regs[reg / 2];
}
void gx_cp_write(uint32_t reg, uint32_t v, int size) {
    reg &= 0x7F;
    if (size == 4) { cp_regs[reg / 2] = v >> 16; cp_regs[reg / 2 + 1] = (uint16_t)v; }
    else cp_regs[reg / 2] = (uint16_t)v;
}
static void pe_update_irq(void) {
    uint16_t ctl = pe_regs[0x0A / 2];
    pi_set(PI_PE_TOKEN, (ctl & 1) && (ctl & 4));
    pi_set(PI_PE_FINISH, (ctl & 2) && (ctl & 8));
}
uint32_t gx_pe_read(uint32_t reg, int size) {
    reg &= 0xFF; (void)size;
    if (reg == 0x0E) return pe_token;
    return pe_regs[reg / 2];
}
void gx_pe_write(uint32_t reg, uint32_t v, int size) {
    reg &= 0xFF; (void)size;
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
uint64_t g_gx_draws, g_gx_cmds;

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

static FILE *fifo_dump; static int fifo_dump_init;
void gx_fifo_write(uint32_t v, int size) {
    hw_pi_fifo_advance(v, size);
    if (!fifo_dump_init) { fifo_dump_init = 1; const char *p = getenv("RT_FIFO_DUMP"); if (p) fifo_dump = fopen(p, "wb"); }
    if (fifo_dump) { for (int i = size - 1; i >= 0; i--) fputc((uint8_t)(v >> (8 * i)), fifo_dump); fflush(fifo_dump); }
    for (int i = size - 1; i >= 0; i--) fifo[fifo_len++] = (uint8_t)(v >> (8 * i));
    /* only execute while the CPU FIFO is the GP's FIFO; otherwise the game is recording a
       display list into memory (GXBeginDisplayList) and the GP must not see it yet */
    uint32_t gp_base = (((uint32_t)cp_regs[0x22 / 2] << 16) | cp_regs[0x20 / 2]) & 0x03FFFFE0;
    if (gp_base != (hw_pi_fifo_base() & 0x03FFFFE0)) { fifo_len -= size; return; }
    if (fifo_len >= 32 || fifo_len >= sizeof fifo - 8) {
        uint32_t used = parse(fifo, fifo_len, 0);
        if (used) { memmove(fifo, fifo + used, fifo_len - used); fifo_len -= used; }
        if (fifo_len > sizeof fifo - 64) rt_fatal("GX FIFO parse stalled (%u bytes pending, first %02X)", fifo_len, fifo[0]);
    }
}

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
