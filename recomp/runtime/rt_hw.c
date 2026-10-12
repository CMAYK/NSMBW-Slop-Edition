/* rt_hw.c - Wii hardware registers: PI, VI, MI, DSP/AI, EXI (RTC/SRAM), SI, write-gather pipe.
 * Registers we don't model are kept as plain storage so reads return what was written. */
#include "rt.h"

static uint8_t regs[0x10000];          /* backing store for 0x0C00xxxx */
static uint8_t regs_hw[0x10000];       /* backing store for 0x0D00xxxx */
static uint32_t unknown_logged;
static void ai_dma_tick(void);

static uint32_t rd_store(uint8_t *s, uint32_t off, int size) {
    if (size == 4) return ((uint32_t)s[off] << 24) | ((uint32_t)s[off + 1] << 16) | ((uint32_t)s[off + 2] << 8) | s[off + 3];
    if (size == 2) return ((uint32_t)s[off] << 8) | s[off + 1];
    return s[off];
}
static void wr_store(uint8_t *s, uint32_t off, uint32_t v, int size) {
    if (size == 4) { s[off] = v >> 24; s[off + 1] = v >> 16; s[off + 2] = v >> 8; s[off + 3] = v; }
    else if (size == 2) { s[off] = v >> 8; s[off + 1] = v; }
    else s[off] = v;
}

/* ------------------------------------------------------------------ PI */
static uint32_t pi_intsr, pi_intmr, pi_fifo_base, pi_fifo_end, pi_fifo_wptr;
void pi_set(uint32_t bits, int on) { if (on) pi_intsr |= bits; else pi_intsr &= ~bits; }
uint32_t pi_active(void) { return pi_intsr & pi_intmr; }

/* ------------------------------------------------------------------ VI */
static uint16_t vi[0x80 / 2];
static uint64_t vi_t0, vi_field;
#define VI_FIELD_NS 16683333ull              /* NTSC field */
static uint32_t vi_di(int i) { return ((uint32_t)vi[(0x30 + 4 * i) / 2] << 16) | vi[(0x30 + 4 * i) / 2 + 1]; }
static void vi_update_irq(void) {
    int any = 0;
    for (int i = 0; i < 4; i++) { uint32_t d = vi_di(i); if ((d & 0x80000000u) && (d & 0x10000000u)) any = 1; }
    pi_set(PI_VI, any);
}
static uint32_t vi_line(void) {
    uint64_t t = (rt_now_ns() - vi_t0) % VI_FIELD_NS;
    return 1 + (uint32_t)(t * 263 / VI_FIELD_NS);
}

uint64_t g_vi_fields;
void hw_update(void) {
    uint64_t f = (rt_now_ns() - vi_t0) / VI_FIELD_NS;
    if (f != vi_field) {
        vi_field = f; g_vi_fields++;
        plat_pump(); { extern void rt_funcstat_tick(void); rt_funcstat_tick(); }
        for (int i = 0; i < 2; i++) {
            int r = (0x30 + 4 * i) / 2;
            if (vi[r] & 0x1000) vi[r] |= 0x8000;      /* raise status if enabled */
        }
        vi_update_irq();
    }
    ai_dma_tick();
    ios_update();
}

/* ------------------------------------------------------------------ EXI (RTC / SRAM) */
static uint32_t exi_reg[3][5];
static uint8_t sram[64];
static uint32_t exi_cmd; static int exi_have_cmd;
static void sram_init(void) {
    memset(sram, 0, sizeof sram);
    /* Dolphin's default SRAM, then checksums over bytes 0x0C-0x13 */
    static const uint8_t def[64] = {
        0xFF, 0x6B, 0x00, 0x91, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x2C, 0x44, 0x4F, 0x4C, 0x50, 0x48, 0x49, 0x4E, 0x53, 0x4C, 0x4F, 0x54, 0x41,
        0x44, 0x4F, 0x4C, 0x50, 0x48, 0x49, 0x4E, 0x53, 0x4C, 0x4F, 0x54, 0x42, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(sram, def, 64);
    uint16_t c1 = 0, c2 = 0;
    for (int i = 0x0C; i < 0x14; i += 2) { uint16_t w = (sram[i] << 8) | sram[i + 1]; c1 += w; c2 += (uint16_t)~w; }
    sram[0] = c1 >> 8; sram[1] = c1; sram[2] = c2 >> 8; sram[3] = c2;
}
static void exi_transfer(int ch) {
    uint32_t cr = exi_reg[ch][3], csr = exi_reg[ch][0];
    int dma = (cr >> 1) & 1, rw = (cr >> 2) & 3, len = ((cr >> 4) & 3) + 1;
    int dev = (csr >> 7) & 7;
    if (ch == 0 && dev == 2) {                   /* RTC/SRAM chip: device 1 => CS bit 1 */
        if (!exi_have_cmd && rw == 1 && !dma) {   /* first immediate write = command */
            exi_cmd = exi_reg[ch][4]; exi_have_cmd = 1;
        } else if (rw == 0) {                     /* read */
            uint32_t off = (exi_cmd >> 6) & 0xFF;
            if ((exi_cmd & 0x7FFFFF00) == 0x20000000 && !(exi_cmd & 0x100)) {   /* RTC */
                uint32_t secs = (uint32_t)(rt_now_ns() / 1000000000ull) + 0x386D4380u;  /* ~2000 epoch */
                exi_reg[ch][4] = secs;
            } else if (dma) {
                uint32_t mar = exi_reg[ch][1], n = exi_reg[ch][2];
                for (uint32_t i = 0; i < n; i++) W8(0x80000000u | (mar + i), i < 64 ? sram[i] : 0);
            } else {
                uint32_t v = 0;
                for (int i = 0; i < len; i++) v |= (uint32_t)sram[(off + i) % 64] << (24 - 8 * i);
                exi_reg[ch][4] = v;
            }
        } else if (dma && rw == 1) {
            uint32_t mar = exi_reg[ch][1], n = exi_reg[ch][2];
            for (uint32_t i = 0; i < n && i < 64; i++) sram[i] = R8(0x80000000u | (mar + i));
        }
    } else if (rw == 0 && !dma) exi_reg[ch][4] = 0;   /* nothing attached */
    exi_reg[ch][3] &= ~1u;                            /* TSTART done */
    exi_reg[ch][0] |= 1u << 3;                        /* TCINT status */
}
static uint32_t exi_read(uint32_t off) {
    int ch = off / 0x14, r = (off % 0x14) / 4;
    if (ch > 2) return 0;
    if (r == 0) return exi_reg[ch][0] | (ch < 2 ? (1u << 12) : 0);   /* EXT: device present */
    return exi_reg[ch][r];
}
static void exi_write(uint32_t off, uint32_t v) {
    int ch = off / 0x14, r = (off % 0x14) / 4;
    if (ch > 2) return;
    if (r == 0) {
        uint32_t old = exi_reg[ch][0];
        uint32_t keep = old & ((1u << 1) | (1u << 3) | (1u << 11));       /* int status bits */
        keep &= ~(v & ((1u << 1) | (1u << 3) | (1u << 11)));              /* write-1-to-clear */
        exi_reg[ch][0] = (v & ~((1u << 1) | (1u << 3) | (1u << 11))) | keep;
        if (((old >> 7) & 7) != ((v >> 7) & 7)) exi_have_cmd = 0;         /* chip select changed */
        return;
    }
    exi_reg[ch][r] = v;
    if (r == 3 && (v & 1)) exi_transfer(ch);
}

/* ------------------------------------------------------------------ DSP / ARAM / AI DMA
 * No DSP code runs: reset/halt are acknowledged instantly, the DSP->CPU mailbox greets
 * with 0x8071FEED after each start, ARAM DMA completes immediately, and audio DMA
 * raises its interrupt at the real playback rate so audio-driven timing keeps ticking. */
static uint16_t dsp[0x200 / 2];
static uint32_t ai_cr, ai_vr, ai_scnt, ai_it;
static uint64_t ai_t0; static uint32_t ai_scnt0;
static uint32_t mail_out[16]; static int nmail_out;
static uint64_t aidma_next_ns;
enum { CSR_RES = 1, CSR_PIINT = 2, CSR_HALT = 4, CSR_AIDINT = 8, CSR_AIDMSK = 0x10, CSR_ARINT = 0x20,
       CSR_ARMSK = 0x40, CSR_DSPINT = 0x80, CSR_DSPMSK = 0x100, CSR_DMABUSY = 0x200 };
static void dsp_update_irq(void) {
    uint16_t c = dsp[0x0A / 2];
    pi_set(PI_DSP, ((c & CSR_AIDINT) && (c & CSR_AIDMSK)) || ((c & CSR_ARINT) && (c & CSR_ARMSK)) || ((c & CSR_DSPINT) && (c & CSR_DSPMSK)));
}
static void dsp_mail(uint32_t m) { if (nmail_out < 16) mail_out[nmail_out++] = m; }
/* high-level DSP: ROM task loader + AX command-list protocol (Dolphin's DSPHLE semantics) */
static int dsp_ucode;                         /* 0 = ROM, 1 = AX */
static uint32_t rom_cmd; static int ax_state; static uint16_t ax_cmdlist_size;
static uint64_t dsp_irq_at;                   /* delayed "work done" interrupt */
static void dsp_mail_irq(uint32_t m, uint64_t delay_ns) {
    dsp_mail(m);
    if (!delay_ns) { dsp[0x0A / 2] |= 0x80; dsp_update_irq(); }
    else dsp_irq_at = rt_now_ns() + delay_ns;
}
/* Advance AX voices like the DSP would (no mixing yet): each command list covers one audio
 * frame; running voices move their playback address, loop, or stop at the end. Without this,
 * streamed music never asks for more data and sounds never finish, which stalls scene changes.
 * Wii AX PB layout (0x140 bytes): +0x10 running, +0x6E looping, +0x70 format, +0x72 loop addr,
 * +0x76 end addr, +0x7A current addr, +0xA6 SRC ratio (16.16), +0xAA fractional position. */
static uint32_t ax_samples_per_frame(void) {
    uint16_t ctl = dsp[0x36 / 2];
    uint32_t n = (uint32_t)(ctl & 0x7FFF) * 32 / 4;
    return (n >= 32 && n <= 1024) ? n : 96;
}
static uint32_t ax_last_list;
/* debug: compare DSP-side PBs with the CPU-side copies inside AXVPBs (pb copy at AXVPB+0x28) */
void rt_ax_scan(void) {
    uint32_t list = ax_last_list; if (!list) return;
    uint32_t pb = ((uint32_t)R16(list + 0x0E) << 16) | R16(list + 0x10);
    for (int k = 0; pb && k < 128; k++) {
        pb |= 0x80000000u;
        uint32_t cpu = 0;
        for (uint32_t a = 0x80300000u; a < 0x80500000u && !cpu; a += 4)
            if (R32(a) == pb && a - 4 != pb && R32(a - 4) == R32(pb)) cpu = a - 0x2C;   /* pb.this == pb, pb.next equal */
        if (cpu || R16(pb + 0x10))
            rt_log("ax", "PB %08X dsp run %d cur %04X%04X end %04X%04X | AXVPB %08X cpu run %d cur %04X%04X sync %08X",
                   pb, R16(pb + 0x10), R16(pb + 0x7A), R16(pb + 0x7C), R16(pb + 0x76), R16(pb + 0x78),
                   cpu, cpu ? R16(cpu + 0x38) : -1, cpu ? R16(cpu + 0x28 + 0x7A) : 0, cpu ? R16(cpu + 0x28 + 0x7C) : 0, cpu ? R32(cpu + 0x1C) : 0);
        pb = ((uint32_t)R16(pb) << 16) | R16(pb + 2);
    }
}
void ax_run_list(uint32_t list);
static void ax_process(uint32_t list) {
    ax_last_list = list;
    { extern uint64_t g_perf_ax_ns; uint64_t t = rt_now_ns(); ax_run_list(list); g_perf_ax_ns += rt_now_ns() - t; } return;
    static const int nargs[8] = {2, 2, 2, 4, 2, 5, 5, 5};
    uint32_t pb = 0;
    for (uint32_t i = 0; i < 64;) {
        uint16_t c = R16(list + 2 * i);
        if (c > 7) return;
        if (c == 4) { pb = ((uint32_t)R16(list + 2 * i + 2) << 16) | R16(list + 2 * i + 4); break; }
        i += 1 + nargs[c];
    }
    uint32_t frame = ax_samples_per_frame();
    for (int k = 0; pb && k < 128; k++) {
        pb |= 0x80000000u;
        if (R16(pb + 0x10)) {
            int fmt = R16(pb + 0x70), loop = R16(pb + 0x6E);
            uint32_t la = ((uint32_t)R16(pb + 0x72) << 16) | R16(pb + 0x74);
            uint32_t ea = ((uint32_t)R16(pb + 0x76) << 16) | R16(pb + 0x78);
            uint32_t ca = ((uint32_t)R16(pb + 0x7A) << 16) | R16(pb + 0x7C);
            uint32_t ratio = ((uint32_t)R16(pb + 0xA6) << 16) | R16(pb + 0xA8);
            uint64_t t = (uint64_t)R16(pb + 0xAA) + (uint64_t)frame * ratio;
            uint32_t n = (uint32_t)(t >> 16);
            W16(pb + 0xAA, (uint16_t)t);
            int stopped = 0;
            while (n--) {
                if (ca >= ea) {
                    /* the accelerator always wraps to the loop address at the end of the sample;
                       one-shot voices then stop. nw4r points one-shot loop addresses at its zero
                       buffer and detects "finished" by the play position landing there. */
                    ca = la;
                    if (!loop) { stopped = 1; break; }
                } else {
                    ca++;
                    if (fmt == 0 && (ca & 15) == 0) ca += 2;   /* skip ADPCM frame header */
                }
            }
            if (stopped) { W16(pb + 0x10, 0); LOG("ax", "voice %08X reached end %08X (fmt %d) -> stopped", pb, ea, fmt); }
            W16(pb + 0x7A, (uint16_t)(ca >> 16)); W16(pb + 0x7C, (uint16_t)ca);
        }
        pb = ((uint32_t)R16(pb) << 16) | R16(pb + 2);
    }
}
static void dsp_handle_mail(uint32_t m) {
    if (dsp_ucode == 0) {
        if (!rom_cmd) { rom_cmd = m; return; }
        uint32_t cmd = rom_cmd; rom_cmd = 0;
        if (cmd == 0x80F3D001) {                   /* start vector: boot the uploaded task */
            LOG("dsp", "task boot (init vector %04X) -> AX HLE", m);
            dsp_ucode = 1; ax_state = 0;
            dsp_mail_irq(0xDCD10000, 0);           /* DSP_INIT */
        }
        return;
    }
    switch (ax_state) {
    case 0:                                       /* waiting for command list size */
        if ((m & 0xFFFF0000) == 0xBABE0000) { ax_cmdlist_size = (uint16_t)m; ax_state = 1; }
        else LOG("dsp", "AX: unexpected mail %08X", m);
        return;
    case 1: {                                     /* command list address: run it (silently) */
        static int nlist; static int dump_at = -2;
        if (dump_at == -2) { const char *e = getenv("RT_AXDUMP"); dump_at = e ? atoi(e) : -1; }
        ++nlist;
        if (dump_at > 0 && nlist % dump_at == 0) {
            uint32_t pb = ((uint32_t)R16(m + 0x0E) << 16) | R16(m + 0x10); int shown = 0;
            fprintf(stderr, "[ax] list %d spf %u:", nlist, ax_samples_per_frame());
            for (int k = 0; pb && k < 96 && shown < 6; k++) {
                pb |= 0x80000000u;
                if (R16(pb + 0x10) && !R16(pb + 0x12) && shown == 0) {
                    fprintf(stderr, "\n  full PB %08X:", pb);
                    for (int i = 0; i < 0xA0; i++) fprintf(stderr, "%s%04X", i % 16 ? " " : "\n   ", R16(pb + 2 * i));
                    fprintf(stderr, "\n ");
                }
                if (R16(pb + 0x10)) { shown++;
                    fprintf(stderr, " [%08X st%d cur %04X%04X end %04X%04X ratio %04X%04X]", pb, R16(pb + 0x12), R16(pb + 0x7A), R16(pb + 0x7C), R16(pb + 0x76), R16(pb + 0x78), R16(pb + 0xA6), R16(pb + 0xA8)); }
                pb = ((uint32_t)R16(pb) << 16) | R16(pb + 2);
            }
            fprintf(stderr, "\n");
        }
        if (0) {
            fprintf(stderr, "[ax] command list %08X size %u:", m, ax_cmdlist_size);
            for (unsigned i = 0; i < ax_cmdlist_size / 2 + 8 && i < 200; i++) fprintf(stderr, "%s%04X", i % 16 ? " " : "\n  ", R16(m + 2 * i));
            fprintf(stderr, "\n");
            uint32_t pb = ((uint32_t)R16(m + 0x0E) << 16) | R16(m + 0x10);   /* CMD_PROCESS argument */
            for (int k = 0; pb && k < 96; k++) {
                if (R16(pb + 0x10)) {
                    fprintf(stderr, "[ax] PB %08X:", pb);
                    for (int i = 0; i < 0xC0; i++) fprintf(stderr, "%s%04X", i % 16 ? " " : "\n  ", R16(pb + 2 * i));
                    fprintf(stderr, "\n");
                }
                pb = ((uint32_t)R16(pb) << 16) | R16(pb + 2);
                if (pb) pb |= 0x80000000u;
            }
        }
        ax_process(m);
        ax_state = 2;
        dsp_mail_irq(0xDCD10002, 50000);           /* DSP_YIELD after a short delay */
        return;
    }
    case 2:
        switch (m | 0xCDD10000) {
        case 0xCDD10000: dsp_mail_irq(0xDCD10001, 0); ax_state = 0; return;   /* resume */
        case 0xCDD10001: ax_state = 0; return;                                 /* new ucode */
        case 0xCDD10002: dsp_ucode = 0; return;                                /* reset to ROM */
        case 0xCDD10003: ax_state = 0; return;                                 /* continue */
        }
        ax_state = 0;
        return;
    }
}
static uint16_t dsp_read(uint32_t r) {
    switch (r) {
    case 0x00: return 0;                                   /* CPU->DSP mailbox: consumed */
    case 0x04: return nmail_out ? (uint16_t)((mail_out[0] >> 16) | 0x8000) : 0;
    case 0x06: {
        if (!nmail_out) return 0;
        uint16_t lo = (uint16_t)mail_out[0];
        memmove(mail_out, mail_out + 1, --nmail_out * 4);
        return lo;
    }
    case 0x0A: return dsp[0x0A / 2] & ~(CSR_RES | CSR_DMABUSY);
    case 0x3A: return 0;                                    /* AI DMA blocks left */
    }
    return dsp[r / 2];
}
static void dsp_write(uint32_t r, uint16_t v) {
    if (r == 0x0A) {
        uint16_t old = dsp[0x0A / 2];
        uint16_t ints = old & (CSR_AIDINT | CSR_ARINT | CSR_DSPINT);
        ints &= ~(v & (CSR_AIDINT | CSR_ARINT | CSR_DSPINT));         /* write 1 to clear */
        dsp[0x0A / 2] = (uint16_t)((v & ~(CSR_AIDINT | CSR_ARINT | CSR_DSPINT | CSR_RES)) | ints);
        if ((old & CSR_HALT) && !(v & CSR_HALT)) { nmail_out = 0; dsp_mail(0x8071FEED); }
        if (v & CSR_RES) { nmail_out = 0; dsp_ucode = 0; rom_cmd = 0; }
        dsp_update_irq();
        return;
    }
    dsp[r / 2] = v;
    if (r == 0x02) { LOG("dsp", "mail to DSP %04X%04X", dsp[0], v); dsp_handle_mail(((uint32_t)dsp[0] << 16) | v); }
    if (r == 0x2A) {                                         /* ARAM DMA count: run it now */
        uint32_t mm = ((uint32_t)dsp[0x20 / 2] << 16) | dsp[0x22 / 2], ar = ((uint32_t)dsp[0x24 / 2] << 16) | dsp[0x26 / 2];
        uint32_t cnt = (((uint32_t)dsp[0x28 / 2] << 16) | v) & 0x7FFFFFFF; int to_main = (dsp[0x28 / 2] >> 15) & 1;
        static uint8_t *aram; if (!aram) aram = calloc(1, 16u << 20);
        ar &= 0xFFFFFF; if (ar + cnt > (16u << 20)) cnt = (16u << 20) - ar;
        if (to_main) memcpy(G(0x80000000u | mm), aram + ar, cnt); else memcpy(aram + ar, G(0x80000000u | mm), cnt);
        dsp[0x0A / 2] |= CSR_ARINT; dsp_update_irq();
    }
}
static void ai_dma_tick(void) {
    if (dsp_irq_at && rt_now_ns() >= dsp_irq_at) { dsp_irq_at = 0; dsp[0x0A / 2] |= 0x80; dsp_update_irq(); }
    uint16_t ctl = dsp[0x36 / 2];
    /* the DMA runs continuously while enabled: re-arming it from the interrupt handler just queues
       the next block, so timing is only re-anchored when it is started from the stopped state
       (re-anchoring on every write lost the handler's latency each block: ~10 % too little audio) */
    if (!(ctl & 0x8000)) { aidma_next_ns = 0; return; }
    uint32_t bytes = (uint32_t)(ctl & 0x7FFF) * 32;
    uint64_t period = (uint64_t)bytes * 1000000000ull / (32000 * 4);
    uint64_t now = rt_now_ns();
    if (!aidma_next_ns) aidma_next_ns = now + period;
    if (now >= aidma_next_ns) {
        /* one buffer has been played: hand it to the host. If the game fell behind, deliver the
           missed periods back to back (each one runs an AX frame) instead of dropping them,
           so audio keeps real-time pace; give up only past 100 ms of backlog. */
        aidma_next_ns += period;
        if (now > aidma_next_ns + 100000000ull) aidma_next_ns = now + period;
        uint32_t src = ((((uint32_t)dsp[0x30 / 2] << 16) | dsp[0x32 / 2]) & 0x03FFFFE0u);
        static int16_t pcm[2 * 4096];
        uint32_t frames = bytes / 4; if (frames > 4096) frames = 4096;
        for (uint32_t i = 0; i < frames; i++) {                 /* buffer holds (right, left) pairs */
            pcm[2 * i + 1] = (int16_t)R16(0x80000000u | (src + 4 * i));
            pcm[2 * i] = (int16_t)R16(0x80000000u | (src + 4 * i + 2));
        }
        plat_audio_push(pcm, (int)frames);
        { static uint64_t st_t0, st_frames, st_resets; st_frames += frames;      /* delivery-rate check, logged every 10 s */
          if (now > aidma_next_ns + 90000000ull) st_resets++;
          if (!st_t0) st_t0 = now;
          else if (now - st_t0 >= 10000000000ull) {
              rt_log("audio", "%.0f samples/s delivered (32000 = real time), DMA block %u frames, backlog resets %llu",
                     st_frames * 1e9 / (double)(now - st_t0), frames, (unsigned long long)st_resets);
              st_t0 = now; st_frames = 0; st_resets = 0; } }
        dsp[0x0A / 2] |= CSR_AIDINT; dsp_update_irq();
    }
}

/* ------------------------------------------------------------------ dispatch */
static void log_unknown(const char *what, uint32_t p, uint32_t v, int size) {
    if (unknown_logged++ < 200) LOG("mmio", "%s %08X (%d bytes) %08X", what, p, size, v);
}

uint32_t mmio_read(uint32_t p, int size) {
    uint32_t off = p & 0xFFFF;
    if (p < 0x0D000000) {
        switch (off & 0xF000) {
        case 0x0000: return gx_cp_read(off & 0xFFF, size);
        case 0x1000: return gx_pe_read(off & 0xFFF, size);
        case 0x2000: {
            uint32_t r = off & 0x7F;
            if (r == 0x2C) vi[0x2C / 2] = vi_line();              /* VCT */
            if (r == 0x2E) vi[0x2E / 2] = 1;
            if (size == 4) return ((uint32_t)vi[r / 2] << 16) | vi[r / 2 + 1];
            return vi[r / 2];
        }
        case 0x3000: {
            uint32_t r = off & 0xFFF;
            if (r == 0x00) return pi_intsr | (1u << 16);           /* reset button not pressed */
            if (r == 0x04) return pi_intmr;
            if (r == 0x0C) return pi_fifo_base;
            if (r == 0x10) return pi_fifo_end;
            if (r == 0x14) return pi_fifo_wptr;
            if (r == 0x2C) return 0x246500B1;                      /* flipper revision */
            return rd_store(regs, off, size);
        }
        case 0x5000: {
            uint32_t r = off & 0x1FF;
            if (size == 4) { uint32_t hi = dsp_read(r); return (hi << 16) | dsp_read(r + 2); }
            return dsp_read(r);
        }
        }
        log_unknown("read ", p, 0, size);
        return rd_store(regs, off, size);
    }
    /* 0x0D00xxxx: Hollywood */
    if (off < 0x400) return ios_mmio_read(off);
    if (off >= 0x6800 && off < 0x6840) return exi_read(off - 0x6800);
    if (off >= 0x6C00 && off < 0x6C20) {
        uint32_t r = off - 0x6C00;
        if (r == 0) return ai_cr;
        if (r == 4) return ai_vr;
        if (r == 8) {                                   /* sample counter runs while playing */
            if (ai_cr & 1) {
                uint64_t rate = (ai_cr & 2) ? 48000 : 32000;
                ai_scnt = ai_scnt0 + (uint32_t)((rt_now_ns() - ai_t0) * rate / 1000000000ull);
            }
            return ai_scnt;
        }
        if (r == 12) return ai_it;
    }
    if (off >= 0x6400 && off < 0x6500) {                           /* SI: no GC controllers */
        uint32_t r = off - 0x6400;
        if (r == 0x38) return 0;                                    /* SISR */
        return rd_store(regs_hw, off, size);
    }
    log_unknown("read ", p, 0, size);
    return rd_store(regs_hw, off, size);
}

void mmio_write(uint32_t p, uint32_t v, int size) {
    uint32_t off = p & 0xFFFF;
    if (p < 0x0D000000) {
        if ((off & 0xF000) == 0x8000) { gx_fifo_write(v, size); return; }   /* write-gather pipe */
        switch (off & 0xF000) {
        case 0x0000: gx_cp_write(off & 0xFFF, v, size); return;
        case 0x1000: gx_pe_write(off & 0xFFF, v, size); return;
        case 0x2000: {
            uint32_t r = off & 0x7F;
            if (size == 4) { vi[r / 2] = v >> 16; vi[r / 2 + 1] = (uint16_t)v; }
            else if (size == 2) vi[r / 2] = (uint16_t)v;
            vi_update_irq();
            return;
        }
        case 0x3000: {
            uint32_t r = off & 0xFFF;
            if (r == 0x00) { pi_intsr &= ~(v & (PI_RSW | PI_GPERR)); return; }
            if (r == 0x04) { pi_intmr = v; return; }
            if (r == 0x0C) { pi_fifo_base = v & 0x3FFFFFE0; pi_fifo_wptr = pi_fifo_base; return; }
            if (r == 0x10) { pi_fifo_end = v & 0x3FFFFFE0; return; }
            if (r == 0x14) { pi_fifo_wptr = v & 0x3FFFFFE0; return; }
            wr_store(regs, off, v, size);
            return;
        }
        case 0x5000: {
            uint32_t r = off & 0x1FF;
            if (size == 4) { dsp_write(r, (uint16_t)(v >> 16)); dsp_write(r + 2, (uint16_t)v); }
            else dsp_write(r, (uint16_t)v);
            return;
        }
        }
        log_unknown("write", p, v, size);
        wr_store(regs, off, v, size);
        return;
    }
    if (off < 0x400) { ios_mmio_write(off, v); return; }
    if (off >= 0x6800 && off < 0x6840) { exi_write(off - 0x6800, v); return; }
    if (off >= 0x6C00 && off < 0x6C20) {
        uint32_t r = off - 0x6C00;
        if (r == 0) {
            if ((ai_cr & 1) && !(v & 1)) {               /* stopping: freeze the counter */
                uint64_t rate = (ai_cr & 2) ? 48000 : 32000;
                ai_scnt = ai_scnt0 + (uint32_t)((rt_now_ns() - ai_t0) * rate / 1000000000ull);
            }
            if (v & 0x20) ai_scnt = 0;                   /* SCRESET */
            if (!(ai_cr & 1) && (v & 1)) { ai_t0 = rt_now_ns(); ai_scnt0 = ai_scnt; }
            ai_cr = (v & ~0x28u) | (ai_cr & 0x8u & ~(v & 0x8u));
        } else if (r == 4) ai_vr = v; else if (r == 8) ai_scnt = v; else if (r == 12) ai_it = v;
        return;
    }
    log_unknown("write", p, v, size);
    wr_store(regs_hw, off, v, size);
}

/* the CPU side of the GX FIFO: also mirror bytes into the PI FIFO ring like the hardware */
void hw_pi_fifo_advance(uint32_t v, int size) {
    if (!pi_fifo_end) return;
    for (int i = size - 1; i >= 0; i--) {
        g_base[pi_fifo_wptr & 0x03FFFFFF] = (uint8_t)(v >> (8 * i));
        pi_fifo_wptr++;
        if ((pi_fifo_wptr & 0x03FFFFFF) >= pi_fifo_end) pi_fifo_wptr = pi_fifo_base | ((pi_fifo_wptr ^ (1u << 26)) & (1u << 26));
    }
}

void hw_init(void) {
    vi_t0 = rt_now_ns();
    sram_init();
}
uint32_t hw_pi_fifo_wptr(void) { return pi_fifo_wptr; }
uint32_t hw_pi_fifo_base(void) { return pi_fifo_base; }
