/* rt_sfx.c - our own sound effects, mixed into the game's audio output.
 *
 * The retro characters' sounds (ui/retro/<character>/sfx/<name>.wav, any rate, 8/16-bit, mono or
 * stereo) are loaded on first use, resampled to the game's 32 kHz stereo and mixed into each block
 * the game's audio DMA hands to the host (rt_hw.c -> sfx_mix -> plat_audio_push). A sound that is
 * started again while it plays restarts (like the NES's sound channels); up to 8 play at once.
 * settings.ini retro_sfx_volume (0-100, default 70). */
#include "rt.h"

#define MAXCLIP 32
#define VOICES 8
typedef struct { char key[96]; int16_t *pcm; int frames; int state; } clip_t;   /* state 0 untried, 1 ok, -1 missing */
static clip_t clips[MAXCLIP];
static int nclips;
static struct { int clip, pos; uint64_t started; } V[VOICES];
static uint64_t started_n;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* a PCM .wav -> 32 kHz stereo int16 (linear resampling); 0 = can't read it */
static int load_wav(const char *path, int16_t **out, int *out_frames) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n > 0 ? n : 1);
    if (n < 44 || fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    if (memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { free(d); return 0; }
    int ch = 0, rate = 0, bits = 0; const uint8_t *data = NULL; uint32_t dlen = 0;
    for (long p = 12; p + 8 <= n; ) {
        uint32_t len = le32(d + p + 4);
        if (!memcmp(d + p, "fmt ", 4) && len >= 16) {
            if (le16(d + p + 8) != 1) { free(d); return 0; }               /* PCM only */
            ch = le16(d + p + 10); rate = (int)le32(d + p + 12); bits = le16(d + p + 22);
        } else if (!memcmp(d + p, "data", 4)) { data = d + p + 8; dlen = len; if (p + 8 + (long)dlen > n) dlen = (uint32_t)(n - p - 8); }
        p += 8 + len + (len & 1);
    }
    if (!data || ch < 1 || ch > 2 || rate < 1000 || (bits != 8 && bits != 16)) { free(d); return 0; }
    int bps = bits / 8 * ch, in_frames = (int)(dlen / bps);
    int frames = (int)((int64_t)in_frames * 32000 / rate);
    int16_t *pcm = malloc(sizeof(int16_t) * 2 * (frames > 0 ? frames : 1));
    for (int i = 0; i < frames; i++) {
        double src = (double)i * rate / 32000.0; int a = (int)src; double t = src - a;
        int b = a + 1 < in_frames ? a + 1 : a;
        for (int c = 0; c < 2; c++) {
            int cc = ch == 2 ? c : 0; double va, vb;
            if (bits == 16) { va = (int16_t)le16(data + a * bps + cc * 2); vb = (int16_t)le16(data + b * bps + cc * 2); }
            else { va = (data[a * bps + cc] - 128) * 256.0; vb = (data[b * bps + cc] - 128) * 256.0; }
            pcm[2 * i + c] = (int16_t)(va + (vb - va) * t);
        }
    }
    free(d);
    *out = pcm; *out_frames = frames;
    return 1;
}

static int clip_of(const char *dir, const char *name) {
    char key[96]; snprintf(key, sizeof key, "%s/%s", dir, name);
    for (int i = 0; i < nclips; i++) if (!strcmp(clips[i].key, key)) return clips[i].state > 0 ? i : -1;
    if (nclips >= MAXCLIP) return -1;
    clip_t *c = &clips[nclips];
    snprintf(c->key, sizeof c->key, "%s", key);
    char path[1300]; snprintf(path, sizeof path, "%s/ui/retro/%s/sfx/%s.wav", g_root, dir, name);
    c->state = load_wav(path, &c->pcm, &c->frames) ? 1 : -1;
    if (c->state < 0) rt_log("sfx", "no %s", path);
    else rt_log("sfx", "%s: %d frames", key, c->frames);
    return nclips++, c->state > 0 ? nclips - 1 : -1;
}

/* play ui/retro/<dir>/sfx/<name>.wav */
void sfx_play(const char *dir, const char *name) {
    if (g_resim) return;                            /* (fields run again after a rollback: heard the first time) */
    int c = clip_of(dir, name);
    if (c < 0) return;
    int v = -1;
    for (int i = 0; i < VOICES; i++) if (V[i].clip == c + 1) v = i;          /* (the same sound: restart it) */
    if (v < 0) for (int i = 0; i < VOICES; i++) if (!V[i].clip) { v = i; break; }
    if (v < 0) { v = 0; for (int i = 1; i < VOICES; i++) if (V[i].started < V[v].started) v = i; }   /* (all busy: the oldest) */
    V[v].clip = c + 1; V[v].pos = 0; V[v].started = ++started_n;
    { static int lg = -1; if (lg < 0) lg = getenv("RT_SFX_LOG") != NULL; if (lg) rt_log("sfx", "play %s/%s", dir, name); }
}
void sfx_stop_all(void) { for (int i = 0; i < VOICES; i++) V[i].clip = 0; }

/* add the playing sounds to a block of 32 kHz stereo */
void sfx_mix(int16_t *pcm, int frames) {
    int any = 0; for (int i = 0; i < VOICES; i++) if (V[i].clip) any = 1;
    if (!any) return;
    static int vol = -1; if (vol < 0) { vol = rt_setting("retro_sfx_volume", 70); if (vol > 100) vol = 100; if (vol < 0) vol = 0; }
    for (int i = 0; i < VOICES; i++) {
        if (!V[i].clip) continue;
        clip_t *c = &clips[V[i].clip - 1];
        int n = c->frames - V[i].pos; if (n > frames) n = frames;
        const int16_t *s = c->pcm + 2 * V[i].pos;
        for (int k = 0; k < 2 * n; k++) {
            int v = pcm[k] + s[k] * vol / 100;
            pcm[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        V[i].pos += n;
        if (V[i].pos >= c->frames) V[i].clip = 0;
    }
}
