/* rt_crypto.c - SHA-256 (file hashes), SHA-512 and Ed25519 signature checks (the update list).
 *
 * The Ed25519 check is TweetNaCl's (public domain: Bernstein, van Gastel, Janssen, Lange,
 * Schwabe, Smetsers), verify side only. Constants: tools/publish_update.py signs with the
 * matching RFC 8032 code. Nothing here is part of a save state. */
#include "rt.h"

/* ------------------------------------------------------------------ SHA-256 */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
void sha256_init(sha256_ctx *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0;
}
void sha256_update(sha256_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data; c->len += n;
    if (c->n) { while (n && c->n < 64) { c->buf[c->n++] = *p++; n--; } if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; } }
    while (n >= 64) { sha256_block(c, p); p += 64; n -= 64; }
    while (n--) c->buf[c->n++] = *p++;
}
void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8; uint8_t pad = 0x80, z = 0;
    sha256_update(c, &pad, 1);
    while (c->n != 56) sha256_update(c, &z, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); sha256_update(c, &b, 1); }
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16); out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i]; }
}

/* ------------------------------------------------------------------ SHA-512 */
typedef struct { uint64_t h[8], len; uint8_t buf[128]; size_t n; } sha512_ctx;
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
static void sha512_block(sha512_ctx *c, const uint8_t *p) {
    uint64_t w[80], v[8];
    for (int i = 0; i < 16; i++) { w[i] = 0; for (int k = 0; k < 8; k++) w[i] = w[i] << 8 | p[8 * i + k]; }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7), s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, c->h, sizeof v);
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = v[7] + (ROR64(v[4], 14) ^ ROR64(v[4], 18) ^ ROR64(v[4], 41)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + K512[i] + w[i];
        uint64_t t2 = (ROR64(v[0], 28) ^ ROR64(v[0], 34) ^ ROR64(v[0], 39)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) c->h[i] += v[i];
}
static void sha512_init(sha512_ctx *c) {
    static const uint64_t iv[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                                   0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0;
}
static void sha512_update(sha512_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data; c->len += n;
    while (n--) { c->buf[c->n++] = *p++; if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; } }
}
static void sha512_final(sha512_ctx *c, uint8_t out[64]) {
    uint64_t bits = c->len * 8; uint8_t pad = 0x80, z = 0;
    sha512_update(c, &pad, 1);
    while (c->n != 112) sha512_update(c, &z, 1);
    for (int i = 0; i < 8; i++) sha512_update(c, &z, 1);   /* (high 64 bits of the length) */
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); sha512_update(c, &b, 1); }
    for (int i = 0; i < 8; i++) for (int k = 0; k < 8; k++) out[8 * i + k] = (uint8_t)(c->h[i] >> (56 - 8 * k));
}

/* ------------------------------------------------------------------ Ed25519 (verify) */
typedef int64_t gf[16];
static const gf gf0 = {0}, gf1 = {1},
    D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406},
    Dc = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203},
    Xb = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169},
    Yb = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666},
    Ic = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
static void set25519(gf r, const gf a) { for (int i = 0; i < 16; i++) r[i] = a[i]; }
static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= (int64_t)((uint64_t)c << 16);
    }
}
static void sel25519(gf p, gf q, int b) {
    int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(uint8_t *o, const gf n) {
    gf m, t; set25519(t, n);
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
static int differ32(const uint8_t *a, const uint8_t *b) { unsigned d = 0; for (int i = 0; i < 32; i++) d |= a[i] ^ b[i]; return d != 0; }
static int neq25519(const gf a, const gf b) { uint8_t c[32], d[32]; pack25519(c, a); pack25519(d, b); return differ32(c, d); }
static uint8_t par25519(const gf a) { uint8_t d[32]; pack25519(d, a); return d[0] & 1; }
static void unpack25519(gf o, const uint8_t *n) { for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8); o[15] &= 0x7fff; }
static void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void M(gf o, const gf a, const gf b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void S(gf o, const gf a) { M(o, a, a); }
static void pow2523(gf o, const gf i) {
    gf c; set25519(c, i);
    for (int a = 250; a >= 0; a--) { S(c, c); if (a != 1) M(c, c, i); }
    set25519(o, c);
}
static void inv25519(gf o, const gf i) {
    gf c; set25519(c, i);
    for (int a = 253; a >= 0; a--) { S(c, c); if (a != 2 && a != 4) M(c, c, i); }
    set25519(o, c);
}
static void padd(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]); Z(t, q[1], q[0]); M(a, a, t);
    A(b, p[0], p[1]); A(t, q[0], q[1]); M(b, b, t);
    M(c, p[3], q[3]); M(c, c, D2);
    M(d, p[2], q[2]); A(d, d, d);
    Z(e, b, a); Z(f, d, c); A(g, d, c); A(h, b, a);
    M(p[0], e, f); M(p[1], h, g); M(p[2], g, f); M(p[3], e, h);
}
static void cswap(gf p[4], gf q[4], uint8_t b) { for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b); }
static void pack(uint8_t *r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]); M(tx, p[0], zi); M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (uint8_t)(par25519(tx) << 7);
}
static void scalarmult(gf p[4], gf q[4], const uint8_t *s) {
    set25519(p[0], gf0); set25519(p[1], gf1); set25519(p[2], gf1); set25519(p[3], gf0);
    for (int i = 255; i >= 0; --i) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b); padd(q, p); padd(p, p); cswap(p, q, b);
    }
}
static void scalarbase(gf p[4], const uint8_t *s) {
    gf q[4]; set25519(q[0], Xb); set25519(q[1], Yb); set25519(q[2], gf1); M(q[3], Xb, Yb);
    scalarmult(p, q, s);
}
static const int64_t Lc[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};
static void modL(uint8_t *r, int64_t x[64]) {
    int64_t carry; int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * Lc[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= (int64_t)((uint64_t)carry << 8);
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) { x[j] += carry - (x[31] >> 4) * Lc[j]; carry = x[j] >> 8; x[j] &= 255; }
    for (j = 0; j < 32; j++) x[j] -= carry * Lc[j];
    for (i = 0; i < 32; i++) { x[i + 1] += x[i] >> 8; r[i] = (uint8_t)(x[i] & 255); }
}
static void reduce(uint8_t *r) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    modL(r, x);
}
static int unpackneg(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]); M(den, num, Dc); Z(num, num, r[2]); A(den, r[2], den);
    S(den2, den); S(den4, den2); M(den6, den4, den2); M(t, den6, num); M(t, t, den);
    pow2523(t, t); M(t, t, num); M(t, t, den); M(t, t, den); M(r[0], t, den);
    S(chk, r[0]); M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], Ic);
    S(chk, r[0]); M(chk, chk, den);
    if (neq25519(chk, num)) return -1;
    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return 0;
}
/* 1 if sig is pk's signature of msg */
int ed25519_verify(const uint8_t sig[64], const void *msg, size_t n, const uint8_t pk[32]) {
    gf p[4], q[4]; uint8_t h[64], t[32];
    {   /* S must be below the group order (no malleable signatures) */
        int lt = 0; for (int i = 31; i >= 0; i--) { if (sig[32 + i] < Lc[i]) { lt = 1; break; } if (sig[32 + i] > Lc[i]) break; }
        if (!lt) return 0; }
    if (unpackneg(q, pk)) return 0;
    sha512_ctx c; sha512_init(&c); sha512_update(&c, sig, 32); sha512_update(&c, pk, 32); sha512_update(&c, msg, n); sha512_final(&c, h);
    reduce(h);
    scalarmult(p, q, h);
    scalarbase(q, sig + 32);
    padd(p, q);
    pack(t, p);
    return !differ32(sig, t);
}
