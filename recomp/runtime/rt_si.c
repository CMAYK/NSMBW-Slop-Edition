/* rt_si.c - the serial interface (SI, 0x0D006400): four GameCube controller ports.
 *
 * The 8-player module reads GameCube controllers as players (its CHAN_GC_0-3 channels, through
 * the SDK's PAD library). Players 5-8 of the port's Wii Remotes come in here (rt_remotes.c gives
 * each port's state); a port with nothing plugged in answers like an empty port.
 *
 * Registers (offsets from 0x6400), as on the hardware (Dolphin's SerialInterface):
 *   0x00/0x0C/0x18/0x24 SICnOUTBUF   the poll command (0x40 0x03 0x00: "standard poll, mode 3")
 *   +4 / +8             SICnINBUFH/L the last poll's 8 bytes (reading H clears the RDST bit)
 *   0x30 SIPOLL     EN bits 7..4 (port 0 = bit 7): which ports the hardware polls each field
 *   0x34 SICOMCSR   TSTART 0, CHANNEL 1-2, INLNGTH 8-14, OUTLNGTH 16-22, RDSTINTMSK 27,
 *                   RDSTINT 28 (read only), COMERR 29, TCINTMSK 30, TCINT 31
 *   0x38 SISR       per port 8 bits (port 0 = bits 24-31): NOREP +3, WRST +4, RDST +5
 *   0x80-0xFF       the transfer buffer
 * A transfer (TSTART) finishes at once: the command is the buffer's first byte - 0x00 / 0xFF
 * type (09 00 00: standard controller), 0x41 / 0x42 origin / recalibrate (10 bytes), 0x40 poll
 * (8 bytes). Polling runs once per video field (si_tick). */
#include "rt.h"
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

static uint32_t outbuf[4], inh[4], inl[4], sipoll, comcsr, sisr, exilk;
static uint8_t buf[128];

#define CSR_TSTART     (1u << 0)
#define CSR_RDSTINTMSK (1u << 27)
#define CSR_RDSTINT    (1u << 28)
#define CSR_COMERR     (1u << 29)
#define CSR_TCINTMSK   (1u << 30)
#define CSR_TCINT      (1u << 31)
#define SR_BIT(port, b) (1u << ((3 - (port)) * 8 + (b)))
enum { SR_NOREP = 3, SR_WRST = 4, SR_RDST = 5 };

static int any_rdst(void) { for (int p = 0; p < 4; p++) if (sisr & SR_BIT(p, SR_RDST)) return 1; return 0; }
static void update_irq(void) {
    int irq = ((comcsr & CSR_TCINT) && (comcsr & CSR_TCINTMSK)) || (any_rdst() && (comcsr & CSR_RDSTINTMSK));
    pi_set(PI_SI, irq);
}

/* a port's 8-byte poll answer: H = [flags | start y x b a][0x80 | l r z up down right left][stick x][stick y],
   L = [c-stick x][c-stick y][trigger l][trigger r] */
static int poll_port(int port, uint32_t *h, uint32_t *l) {
    uint8_t s[8];
    if (!remotes_gc_state(port, s)) return 0;
    *h = ((uint32_t)s[0] << 24) | ((uint32_t)(s[1] | 0x80) << 16) | ((uint32_t)s[2] << 8) | s[3];
    *l = ((uint32_t)s[4] << 24) | ((uint32_t)s[5] << 16) | ((uint32_t)s[6] << 8) | s[7];
    return 1;
}

static void transfer(void) {
    int port = (comcsr >> 1) & 3, inlen = (comcsr >> 8) & 0x7F;
    if (!inlen) inlen = 128;
    uint8_t cmd = buf[0];
    uint8_t s[8];

    int present = remotes_gc_state(port, s);
    comcsr &= ~(CSR_TSTART | CSR_COMERR);
    if (!present) {                                         /* nothing plugged in */
        sisr |= SR_BIT(port, SR_NOREP);
        comcsr |= CSR_COMERR;
    } else {
        sisr &= ~SR_BIT(port, SR_NOREP);
        uint8_t r[10]; int n = 0;
        switch (cmd) {
        case 0x00: case 0xFF: r[0] = 0x09; r[1] = 0x00; r[2] = 0x00; n = 3; break;            /* standard controller */
        case 0x41: case 0x42: {                                                               /* origin: centred, triggers low */
            static const uint8_t org[10] = {0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x1F, 0x1F, 0x00, 0x00};
            memcpy(r, org, 10); n = 10; break;
        }
        case 0x40: { uint32_t h, l; poll_port(port, &h, &l);
            r[0] = h >> 24; r[1] = h >> 16; r[2] = h >> 8; r[3] = h; r[4] = l >> 24; r[5] = l >> 16; r[6] = l >> 8; r[7] = l; n = 8; break; }
        default: rt_log("si", "port %d: unknown command %02X", port, cmd); break;
        }
        if (n > inlen) n = inlen;
        memcpy(buf, r, n);
    }
    comcsr |= CSR_TCINT;
    update_irq();
}

uint32_t si_read(uint32_t r) {
    if (r < 0x30) {
        int port = r / 0xC, k = r % 0xC;
        if (k == 0) return outbuf[port];
        if (k == 4) { sisr &= ~SR_BIT(port, SR_RDST); update_irq(); return inh[port]; }
        return inl[port];
    }
    if (r == 0x30) return sipoll;
    if (r == 0x34) return (comcsr & ~CSR_RDSTINT) | (any_rdst() ? CSR_RDSTINT : 0);
    if (r == 0x38) return sisr;
    if (r == 0x3C) return exilk;
    if (r >= 0x80 && r < 0x100) return ((uint32_t)buf[r - 0x80] << 24) | ((uint32_t)buf[r - 0x80 + 1] << 16) | ((uint32_t)buf[r - 0x80 + 2] << 8) | buf[r - 0x80 + 3];
    return 0;
}
void si_write(uint32_t r, uint32_t v) {
    if (r < 0x30) { int port = r / 0xC, k = r % 0xC; if (k == 0) outbuf[port] = v; return; }
    if (r == 0x30) { sipoll = v; return; }
    if (r == 0x34) {
        if (v & CSR_TCINT) comcsr &= ~CSR_TCINT;                                       /* write 1 clears */
        comcsr = (comcsr & (CSR_TCINT | CSR_COMERR)) | (v & ~(CSR_TCINT | CSR_RDSTINT | CSR_COMERR));
        update_irq();
        if (v & CSR_TSTART) transfer();
        return;
    }
    if (r == 0x38) {                                                                   /* error bits: write 1 clears */
        for (int p = 0; p < 4; p++) sisr &= ~(v & (0x0Fu << ((3 - p) * 8)));
        return;
    }
    if (r == 0x3C) { exilk = v; return; }
    if (r >= 0x80 && r < 0x100) { int o = r - 0x80; buf[o] = v >> 24; buf[o + 1] = v >> 16; buf[o + 2] = v >> 8; buf[o + 3] = v; }
}
/* 8- or 16-bit access to the buffer (byte-wise readers) */
uint32_t si_read_sized(uint32_t r, int size) {
    if (size == 4 || r < 0x80) return si_read(r & ~3u);
    if (r >= 0x100) return 0;
    if (size == 2) return ((uint32_t)buf[r - 0x80] << 8) | buf[r - 0x80 + 1];
    return buf[r - 0x80];
}
void si_write_sized(uint32_t r, uint32_t v, int size) {
    if (size == 4 || r < 0x80) { si_write(r & ~3u, v); return; }
    if (r >= 0x100) return;
    if (size == 2) { buf[r - 0x80] = v >> 8; buf[r - 0x80 + 1] = v; } else buf[r - 0x80] = v;
}

/* once per video field: the hardware polls the enabled ports */
void si_tick(void) {
    for (int p = 0; p < 4; p++) {
        if (!(sipoll & (0x80u >> p))) continue;
        uint32_t h, l;
        if (poll_port(p, &h, &l)) { inh[p] = h; inl[p] = l; sisr &= ~SR_BIT(p, SR_NOREP); sisr |= SR_BIT(p, SR_RDST); }
        else sisr |= SR_BIT(p, SR_NOREP);                                           /* no controller */
    }
    update_irq();
}

/* SITransfer: a port's first transfer is held until "the last transfer + the delay" (XferTime,
   0 at boot) - but the system time is negative early on here (0x800030D8 = -1 high word), so
   that time lies about a minute ahead and the type probes wait for it. A port that never
   transferred counts as long ago. */
#define SI_XFER_TIME 0x8038F850u        /* OSTime XferTime[4] */
void hle_SITransfer(ppc_context *ctx, uint8_t *base) {
    uint32_t chan = ctx->r[3] & 3, t = SI_XFER_TIME + 8 * chan;
    if (!R32(t) && !R32(t + 4)) W32(t, 0x80000000u);
    orig_SITransfer(ctx, base);
}
