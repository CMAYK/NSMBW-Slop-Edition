/* rt_ax.c - high-level emulation of the Wii "AX" DSP microcode (the audio mixer).
 *
 * Each AX frame (3 ms = 96 samples at 32 kHz) the game sends a command list. We run it like
 * the DSP would (semantics follow Dolphin's AXWii HLE):
 *   PROCESS     walk the voice parameter blocks (PBs): fetch samples through the "accelerator"
 *               (ADPCM / PCM8 / PCM16, loops, end -> jump to loop address and stop), resample,
 *               apply the volume envelope and mix into main / AUX A,B,C (L, R, surround)
 *   MIX_AUXx    hand an AUX mix to the CPU (effects such as reverb run there) and mix back what
 *               the CPU produced
 *   OUTPUT      write the final stereo frame to main memory, where the audio DMA plays it from
 * Not emulated: remote-speaker mixes, low-pass/biquad filters, initial time delay, DPL2. */
#include "rt.h"
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

/* PB (parameter block) layout, Wii AX (0x140 bytes) */
enum {
    PB_NEXT = 0x00, PB_SRCTYPE = 0x08, PB_COEFSEL = 0x0A, PB_RUNNING = 0x10, PB_ISSTREAM = 0x12,
    PB_MIXER = 0x14,              /* 12 x {u16 volume, s16 delta}: L R aAL aAR aBL aBR aCL aCR S aAS aBS aCS */
    PB_VE = 0x6A,                 /* u16 current volume, s16 delta */
    PB_LOOP = 0x6E, PB_FORMAT = 0x70, PB_LOOPADDR = 0x72, PB_ENDADDR = 0x76, PB_CURADDR = 0x7A,
    PB_COEFS = 0x7E, PB_PS = 0xA0, PB_YN1 = 0xA2, PB_YN2 = 0xA4,
    PB_RATIO = 0xA6, PB_FRAC = 0xAA, PB_LAST = 0xAC,       /* last_samples[4] */
    PB_LOOP_PS = 0xB4, PB_LOOP_YN1 = 0xB6, PB_LOOP_YN2 = 0xB8,
};
#define N 96                       /* samples per AX frame */
enum { MAIN_L, MAIN_R, MAIN_S, AXA_L, AXA_R, AXA_S, AXB_L, AXB_R, AXB_S, AXC_L, AXC_R, AXC_S, NBUF };
static int32_t mix[NBUF][N];

static inline uint32_t r32(uint32_t a) { return ((uint32_t)R16(a) << 16) | R16(a + 2); }
static inline void w32(uint32_t a, uint32_t v) { W16(a, (uint16_t)(v >> 16)); W16(a + 2, (uint16_t)v); }
static inline int16_t clamp16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }

/* ---- accelerator: sample fetch with ADPCM decode and loop/end handling ---- */
typedef struct {
    uint32_t pb, cur, end, loop; int fmt, looping, stream, stopped;
    int ps, yn1, yn2; int16_t coef[16];
} accel;

static int16_t accel_read(accel *a) {
    if (a->stopped) return 0;
    int16_t s;
    switch (a->fmt) {
    case 0x00: {                                       /* 4-bit ADPCM: 8-byte frames, 14 samples each */
        if ((a->cur & 15) == 0) { a->ps = R8(0x80000000u | (a->cur >> 1)); a->cur += 2; }
        uint8_t byte = R8(0x80000000u | (a->cur >> 1));
        int nib = (a->cur & 1) ? (byte & 15) : (byte >> 4); if (nib & 8) nib -= 16;
        int scale = 1 << (a->ps & 15), ci = (a->ps >> 4) & 7;
        int32_t v = ((nib * scale) << 11) + a->coef[ci * 2] * a->yn1 + a->coef[ci * 2 + 1] * a->yn2;
        v = (v + 1024) >> 11;
        s = clamp16(v); a->yn2 = a->yn1; a->yn1 = s;
        break;
    }
    case 0x0A: s = (int16_t)R16(0x80000000u | (a->cur * 2)); break;            /* PCM16 */
    case 0x19: s = (int16_t)((int8_t)R8(0x80000000u | a->cur) << 8); break;    /* PCM8 */
    default: s = 0; break;
    }
    if (a->cur >= a->end) {
        /* end of sample: the accelerator jumps to the loop address; one-shot voices stop there.
           (nw4r detects "finished" by the position landing in its zero buffer = loop address) */
        a->cur = a->loop;
        if (a->looping) {
            if (!a->stream && a->fmt == 0) { a->ps = R16(a->pb + PB_LOOP_PS); a->yn1 = (int16_t)R16(a->pb + PB_LOOP_YN1); a->yn2 = (int16_t)R16(a->pb + PB_LOOP_YN2); }
        } else a->stopped = 1;
    } else a->cur++;
    return s;
}

/* ---- one voice ---- */
static void process_voice(uint32_t pb) {
    accel a; memset(&a, 0, sizeof a);
    a.pb = pb; a.fmt = R16(pb + PB_FORMAT); a.looping = R16(pb + PB_LOOP); a.stream = R16(pb + PB_ISSTREAM);
    a.loop = r32(pb + PB_LOOPADDR); a.end = r32(pb + PB_ENDADDR); a.cur = r32(pb + PB_CURADDR);
    a.ps = R16(pb + PB_PS); a.yn1 = (int16_t)R16(pb + PB_YN1); a.yn2 = (int16_t)R16(pb + PB_YN2);
    for (int i = 0; i < 16; i++) a.coef[i] = (int16_t)R16(pb + PB_COEFS + 2 * i);

    /* resample (linear) using the PB's 4-sample history and fractional position */
    uint32_t ratio = r32(pb + PB_RATIO), pos = R16(pb + PB_FRAC);
    int16_t h[4]; for (int i = 0; i < 4; i++) h[i] = (int16_t)R16(pb + PB_LAST + 2 * i);
    int32_t out[N];
    if (ratio > 0x100000) ratio = 0x100000;
    for (int i = 0; i < N; i++) {
        pos += ratio;
        while (pos >= 0x10000) { pos -= 0x10000; h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = accel_read(&a); }
        out[i] = h[2] + (((int32_t)h[3] - h[2]) * (int32_t)pos >> 16);
    }
    W16(pb + PB_FRAC, (uint16_t)pos);
    for (int i = 0; i < 4; i++) W16(pb + PB_LAST + 2 * i, (uint16_t)h[i]);

    /* volume envelope */
    int32_t ve = R16(pb + PB_VE), ved = (int16_t)R16(pb + PB_VE + 2);
    for (int i = 0; i < N; i++) {
        out[i] = (out[i] * ve) >> 15;
        ve += ved; if (ve < 0) ve = 0; if (ve > 0xFFFF) ve = 0xFFFF;
    }
    W16(pb + PB_VE, (uint16_t)ve);

    /* mix: PB mixer order L R aAL aAR aBL aBR aCL aCR S aAS aBS aCS */
    static const int dst[12] = {MAIN_L, MAIN_R, AXA_L, AXA_R, AXB_L, AXB_R, AXC_L, AXC_R, MAIN_S, AXA_S, AXB_S, AXC_S};
    for (int m = 0; m < 12; m++) {
        uint32_t va = pb + PB_MIXER + 4 * m;
        int32_t vol = R16(va), d = (int16_t)R16(va + 2);
        if (!vol && !d) continue;
        int32_t *o = mix[dst[m]];
        for (int i = 0; i < N; i++) { o[i] += (out[i] * vol) >> 15; vol += d; if (vol < 0) vol = 0; if (vol > 0xFFFF) vol = 0xFFFF; }
        W16(va, (uint16_t)vol);
    }

    /* write back the accelerator state */
    w32(pb + PB_CURADDR, a.cur);
    W16(pb + PB_PS, (uint16_t)a.ps); W16(pb + PB_YN1, (uint16_t)a.yn1); W16(pb + PB_YN2, (uint16_t)a.yn2);
    if (a.stopped) W16(pb + PB_RUNNING, 0);
}

static void process_pbs(uint32_t pb) {
    for (int k = 0; pb && k < 128; k++) {
        pb |= 0x80000000u;
        if (R16(pb + PB_RUNNING)) process_voice(pb);
        pb = r32(pb + PB_NEXT);
    }
}

static void mix_aux(int aux, uint16_t volume, uint32_t up, uint32_t down) {
    int base = AXA_L + aux * 3;
    if (up) {                                          /* upload L, R, S as big-endian s32 */
        uint32_t p = up | 0x80000000u;
        for (int b = 0; b < 3; b++) for (int i = 0; i < N; i++, p += 4) W32(p, (uint32_t)mix[base + b][i]);
    }
    if (down) {                                        /* the CPU's processed mix (effects output) */
        uint32_t p = down | 0x80000000u;
        for (int b = 0; b < 3; b++) for (int i = 0; i < N; i++, p += 4) mix[MAIN_L + b][i] += (int32_t)(((int64_t)(int32_t)R32(p) * volume) >> 15);
    }
}

static void output(uint16_t volume, uint32_t surround, uint32_t lr) {
    if (surround) { uint32_t p = surround | 0x80000000u; for (int i = 0; i < N; i++, p += 4) W32(p, (uint32_t)((mix[MAIN_S][i] * volume) >> 15)); }
    if (lr) {
        uint32_t p = lr | 0x80000000u;
        for (int i = 0; i < N; i++, p += 4) {           /* right, left (the order the audio DMA expects) */
            W16(p, (uint16_t)clamp16((mix[MAIN_R][i] * volume) >> 15));
            W16(p + 2, (uint16_t)clamp16((mix[MAIN_L][i] * volume) >> 15));
        }
    }
}

/* run one command list */
void ax_run_list(uint32_t list) {
    static const int8_t nargs[16] = {2, 2, 2, 4, 2, 5, 5, 5, -1, -1, 4, 5, -1, 8, 0, -1};
    memset(mix, 0, sizeof mix);
    uint32_t p = list | 0x80000000u;
    for (int guard = 0; guard < 64; guard++) {
        uint16_t c = R16(p); p += 2;
        if (c > 15 || nargs[c] < 0) { LOG("ax", "unknown AX command %02X", c); return; }
        uint16_t w[8]; for (int i = 0; i < nargs[c]; i++) { w[i] = R16(p); p += 2; }
        switch (c) {
        case 0x04: process_pbs(((uint32_t)w[0] << 16) | w[1]); break;
        case 0x05: case 0x06: case 0x07:
            mix_aux(c - 5, w[0], ((uint32_t)w[1] << 16) | w[2], ((uint32_t)w[3] << 16) | w[4]); break;
        case 0x0B: output(w[0], ((uint32_t)w[1] << 16) | w[2], ((uint32_t)w[3] << 16) | w[4]); break;
        case 0x0E: return;
        default: break;                                /* setup, LR adds, remote speakers: ignored */
        }
    }
}
