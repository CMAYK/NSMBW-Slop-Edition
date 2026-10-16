/* rt_net.c - online play, phase 1: the connection (docs/ONLINE.md).
 *
 * Host-authoritative client-server over UDP, direct IP (the host's port forwarded). The host's
 * game is the real one. Each client sends its controllers' buttons every frame; the host plays
 * them in player slots of its own (rt_remotes.c: slot kind "online"). Later phases send the
 * host's world back (scene flow, player snapshots with client-side prediction and
 * reconciliation, then enemies / items as puppets).
 *
 * Packets: an 8-byte header (magic "NWN1", type, protocol version, payload length) + payload,
 * little-endian.
 *   HELLO   c->h  name[16], players u8 (local controllers), version u32 (rt_version: must match the host's),
 *                 nonce u32, then per player: username[4], preferred character u8
 *   WELCOME h->c  ... + the HELLO's nonce u32 at the end (accepted from another address than
 *                 the one joined: NAT loopback / a host with two networks)
 *   WELCOME h->c  client id u8, players u8, slots[4] u8, host frame u32
 *   REJECT  h->c  reason text
 *   INPUT   c->h  newest frame u32, count u8 (<= 8, newest first), players u8,
 *                 then count x players x buttons u32 - every frame, the last 8 frames again
 *                 (a lost packet costs nothing)
 *   STATE   h->c  host frame u32, your newest input used u32, players u8, then per player:
 *                 slot u8, kind u8 (0 host, 1 online), ping u16, name[16]
 *   PING / PONG   time u64 (round trip -> ping)
 *   BYE           either way
 * The host plays each client's inputs in order, one per frame, a couple of frames behind the
 * newest it has (a small jitter buffer); a missing frame repeats the last buttons.
 *
 * Testing (Linux): RT_NET_HOST=port, RT_NET_JOIN=ip:port, RT_NET_FAKE_INPUT="frame:hex:frames,..."
 * (a client presses those buttons). Log lines [net]. */
#include "rt.h"
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
#endif

#define NET_MAGIC 0x314E574Eu   /* "NWN1" */
#define NET_VER   1
#define MAXCL     7
#define MAXLOCAL  4
#define HIST      64
#define NOMIS 0xFFFFFFFFu
enum { NM_HELLO = 1, NM_WELCOME, NM_REJECT, NM_INPUT, NM_STATE, NM_PING, NM_PONG, NM_BYE };
enum { NM_OFFER = 20, NM_GETCHUNK, NM_CHUNK, NM_READY, NM_GO_UNUSED, NM_SHELLO_UNUSED, NM_SWELCOME_UNUSED, NM_SINPUT, NM_SFRAME, NM_SHASH, NM_SDESYNC };

typedef struct { uint32_t frame; uint32_t b[MAXLOCAL]; int have; } inframe_t;
typedef struct {
    int active;
    struct sockaddr_in addr;
    char name[17];
    int players, slots[MAXLOCAL];
    uint64_t last_heard;
    int ping, pings[6], npings;          /* (the last few round trips: the online game's input delay) */
    inframe_t ring[HIST];                /* by frame % HIST */
    uint32_t newest, next;               /* newest frame received, next to play */
    int started;
    uint32_t held[MAXLOCAL];             /* buttons being played */
    char tag[MAXLOCAL][5]; int pref[MAXLOCAL];   /* its players' profiles: usernames, preferred characters */
} client_t;

static int role;                         /* NET_OFF / NET_HOST / NET_JOINING / NET_CLIENT */
static sock_t sk = BAD_SOCK;
static int port;
static char status[160];
/* host */
static client_t C[MAXCL];
/* client */
static struct sockaddr_in host_addr;
static int my_id, my_players, my_slots[MAXLOCAL];
static uint64_t hello_t0, hello_last, host_heard, ping_sent;
static int my_ping;
static uint32_t sent_frames[HIST][MAXLOCAL];
static uint32_t acked_frame;
static struct { int slot, kind, ping; char name[17]; } lobby[8];
static int nlobby;

static uint64_t ms(void) { return rt_now_ns() / 1000000ull; }
static void set_status(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(status, sizeof status, fmt, ap); va_end(ap);
    rt_log("net", "%s", status);
}

/* ---- sockets ---- */
static int sock_open(int bind_port) {
#ifdef _WIN32
    static int wsa; if (!wsa) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) { set_status("No network (WSAStartup failed)"); return 0; } wsa = 1; }
#endif
    sk = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sk == BAD_SOCK) { set_status("Can't open a network socket"); return 0; }
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)bind_port);
    if (bind(sk, (struct sockaddr *)&a, sizeof a)) { close_sock(sk); sk = BAD_SOCK; set_status("Port %d is in use", bind_port); return 0; }
    { int sz = 4 << 20; setsockopt(sk, SOL_SOCKET, SO_RCVBUF, (const char *)&sz, sizeof sz); setsockopt(sk, SOL_SOCKET, SO_SNDBUF, (const char *)&sz, sizeof sz); }   /* (a game's pages arrive in bursts) */
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(sk, FIONBIO, &nb);
    { DWORD no = FALSE; DWORD br; WSAIoctl(sk, 0x9800000C /* SIO_UDP_CONNRESET */, &no, sizeof no, NULL, 0, &br, NULL, NULL); }   /* (an ICMP "port unreachable" mustn't break recv) */
#else
    fcntl(sk, F_SETFL, fcntl(sk, F_GETFL) | O_NONBLOCK);
#endif
    return 1;
}
static void sock_close(void) { if (sk != BAD_SOCK) close_sock(sk); sk = BAD_SOCK; }
static uint64_t blip_until;           /* testing (RT_NET_BLIP_AT): the network "drops" until then */
static uint64_t blip_out_until;       /* testing (RT_NET_BLIP_OUT_AT): only what this PC sends is lost */
static uint64_t ms(void);
/* testing: RT_NET_LAG="ms[:jitter ms]" holds every packet this PC sends that long (a far-away
   player on one PC: give both copies half the round trip) */
#define LAGQ 8192
static struct { uint64_t due; struct sockaddr_in to; int len; uint8_t buf[1408]; } lagq[LAGQ] RT_LOCAL;
static int lag_head RT_LOCAL, lag_tail RT_LOCAL, lag_ms RT_LOCAL = -1, lag_jit RT_LOCAL;
static void lag_flush(void) {
    uint64_t now = ms();
    while (lag_tail != lag_head && lagq[lag_tail].due <= now) {
        if (sk != BAD_SOCK) sendto(sk, (const char *)lagq[lag_tail].buf, lagq[lag_tail].len, 0, (const struct sockaddr *)&lagq[lag_tail].to, sizeof lagq[lag_tail].to);
        lag_tail = (lag_tail + 1) % LAGQ;
    }
}
static void lag_drain(void) {                      /* (closing: what's held still goes out) */
    uint64_t end = ms() + (uint64_t)(lag_ms > 0 ? lag_ms + lag_jit + 50 : 0);
    while (lag_tail != lag_head && ms() < end) { lag_flush(); plat_idle_wait(2000000); }
}
static void note_ping(client_t *c, int p) {
    c->ping = p; c->pings[c->npings++ % 6] = p;
}
static int ping_high(const client_t *c) {          /* the worst of the last few */
    int n = c->npings < 6 ? c->npings : 6, hi = c->ping;
    for (int i = 0; i < n; i++) if (c->pings[i] > hi) hi = c->pings[i];
    return hi;
}
static void send_to(const struct sockaddr_in *to, int type, const uint8_t *payload, int len) {
    if (sk == BAD_SOCK) return;
    if (blip_until && ms() < blip_until) return;
    if (blip_out_until && ms() < blip_out_until) return;
    if (lag_ms < 0) { const char *e = getenv("RT_NET_LAG"); lag_ms = 0; if (e) sscanf(e, "%d:%d", &lag_ms, &lag_jit); }
    if (lag_ms > 0 && len + 8 <= 1408 && (lag_head + 1) % LAGQ != lag_tail) {
        uint8_t *b = lagq[lag_head].buf; uint32_t m = NET_MAGIC;
        b[0] = (uint8_t)m; b[1] = (uint8_t)(m >> 8); b[2] = (uint8_t)(m >> 16); b[3] = (uint8_t)(m >> 24);
        b[4] = (uint8_t)type; b[5] = NET_VER; b[6] = (uint8_t)len; b[7] = (uint8_t)(len >> 8);
        if (len) memcpy(b + 8, payload, len);
        uint64_t due = ms() + (uint64_t)lag_ms + (lag_jit > 0 ? (uint64_t)(rand() % (lag_jit + 1)) : 0);
        int prev = (lag_head + LAGQ - 1) % LAGQ;   /* (UDP may reorder, but keep it simple: in order) */
        if (lag_head != lag_tail && lagq[prev].due > due) due = lagq[prev].due;
        lagq[lag_head].due = due; lagq[lag_head].to = *to; lagq[lag_head].len = len + 8;
        lag_head = (lag_head + 1) % LAGQ;
        return;
    }
    uint8_t buf[1500];
    if (len > (int)sizeof buf - 8) return;
    uint32_t m = NET_MAGIC;
    buf[0] = (uint8_t)m; buf[1] = (uint8_t)(m >> 8); buf[2] = (uint8_t)(m >> 16); buf[3] = (uint8_t)(m >> 24);
    buf[4] = (uint8_t)type; buf[5] = NET_VER; buf[6] = (uint8_t)len; buf[7] = (uint8_t)(len >> 8);
    if (len) memcpy(buf + 8, payload, len);
    sendto(sk, (const char *)buf, len + 8, 0, (const struct sockaddr *)to, sizeof *to);
}
static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) { return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port; }

/* little-endian writers / readers */
typedef struct { uint8_t *p; int n, cap; } wbuf;
static void w8(wbuf *w, uint32_t v) { if (w->n < w->cap) w->p[w->n++] = (uint8_t)v; }
static void w16(wbuf *w, uint32_t v) { w8(w, v); w8(w, v >> 8); }
static void w32(wbuf *w, uint32_t v) { w16(w, v); w16(w, v >> 16); }
static void w64(wbuf *w, uint64_t v) { w32(w, (uint32_t)v); w32(w, (uint32_t)(v >> 32)); }
static void wstr(wbuf *w, const char *s, int n) { for (int i = 0; i < n; i++) w8(w, s && i < (int)strlen(s) ? (uint8_t)s[i] : 0); }
typedef struct { const uint8_t *p; int n, i, bad; } rbuf;
static uint32_t r8(rbuf *r) { if (r->i >= r->n) { r->bad = 1; return 0; } return r->p[r->i++]; }
static uint32_t r16(rbuf *r) { uint32_t a = r8(r); return a | r8(r) << 8; }
static uint32_t r32(rbuf *r) { uint32_t a = r16(r); return a | r16(r) << 16; }
static uint64_t r64(rbuf *r) { uint64_t a = r32(r); return a | (uint64_t)r32(r) << 32; }
static void rstr(rbuf *r, char *s, int n) { for (int i = 0; i < n; i++) s[i] = (char)r8(r); s[n] = 0; for (int i = 0; i < n; i++) if (s[i] && (s[i] < 0x20 || s[i] > 0x7E)) s[i] = '?'; }

static const char *my_name(void) {
    static char n[17]; static int done;
    {   char t[5]; int pref; prof_local_player(0, t, &pref);    /* (player 1's profile, if it has one) */
        if (t[0]) { snprintf(n, sizeof n, "%s", t); return n; } }
    if (!done) { done = 1;
        char p[1200]; snprintf(p, sizeof p, "%s/online.ini", g_root); FILE *f = fopen(p, "r");
        snprintf(n, sizeof n, "Player");
        if (f) { char line[128]; while (fgets(line, sizeof line, f)) { char v[64]; if (sscanf(line, " name = %16[^\r\n]", v) == 1) snprintf(n, sizeof n, "%s", v); } fclose(f); } }
    return n;
}

static void host_lobby_packet(client_t *c, int type, rbuf *r);
static void client_lobby_packet(int type, rbuf *r);
static void host_offer_frame(void);
static void client_lobby_frame(void);
static void session_poll(void);
static client_t *client_by_addr(const struct sockaddr_in *a);
static void test_start(void);
void plat_idle_wait(uint64_t ns);
/* ---- host ---- */
int net_host(int p) {
    net_leave();
    port = p;
    if (!sock_open(p)) return 0;
    memset(C, 0, sizeof C);
    role = NET_HOST;
    set_status("Hosting on port %d - waiting for players", p);
    return 1;
}
static client_t *client_by_addr(const struct sockaddr_in *a) { for (int i = 0; i < MAXCL; i++) if (C[i].active && same_addr(&C[i].addr, a)) return &C[i]; return NULL; }
static void drop_client(client_t *c, const char *why) {
    for (int k = 0; k < c->players; k++) if (c->slots[k] >= 0) remotes_net_release(c->slots[k]);
    set_status("%s left (%s)", c->name, why);
    c->active = 0;
}
/* HELLO's build field: the game version (both sides must run the same one; old builds sent 1) */
static uint32_t version_code(const char *v) {
    unsigned a = 0, b = 0, c = 0; sscanf(v, "%u.%u.%u", &a, &b, &c);
    return 0x80000000u | (a & 0x7F) << 16 | (b & 0xFF) << 8 | (c & 0xFF);
}
static int version_refused(const struct sockaddr_in *from, uint32_t build) {
    if (build == version_code(rt_version())) return 0;
    char v[24], t[100];
    if (build & 0x80000000u) snprintf(v, sizeof v, "%u.%u.%u", (build >> 16) & 0x7F, (build >> 8) & 0xFF, build & 0xFF);
    else snprintf(v, sizeof v, "older");
    snprintf(t, sizeof t, "Different game versions (host %s, you %s) - restart both games to update", rt_version(), v);
    send_to(from, NM_REJECT, (const uint8_t *)t, (int)strlen(t));
    rt_log("net", "refused a player with version %s", v);
    return 1;
}
/* HELLO's nonce (after name, players, version): the WELCOME echoes it, so a joiner knows the
   answer is the host's even when it comes from another address than the one it typed (the
   host's router sending its LAN-side address back - "NAT loopback" - or a second network) */
static uint32_t hello_nonce(rbuf r) { char nm[17]; rstr(&r, nm, 16); r8(&r); r32(&r); uint32_t n = r32(&r); return r.bad ? 0 : n; }
static void host_packet(const struct sockaddr_in *from, int type, rbuf *r) {
    client_t *c = client_by_addr(from);
    if (type == NM_HELLO) {
        char name[17]; rstr(r, name, 16); int players = (int)r8(r); uint32_t build = r32(r), nonce = r32(r);
        if (r->bad) return;
        if (!c && version_refused(from, build)) return;
        char tags[MAXLOCAL][5]; int prefs[MAXLOCAL];             /* (newer joiners: each player's profile) */
        for (int k = 0; k < MAXLOCAL; k++) { tags[k][0] = 0; prefs[k] = -1; }
        for (int k = 0; k < MAXLOCAL && r->i + 5 <= r->n; k++) { rstr(r, tags[k], 4); int p = (int)r8(r); prefs[k] = p == 0xFF ? -1 : p; }
        if (c) { /* (our WELCOME got lost: again) */ }
        else {
            int i; for (i = 0; i < MAXCL && C[i].active; i++) {}
            if (i == MAXCL) { const char *t = "The game is full"; send_to(from, NM_REJECT, (const uint8_t *)t, (int)strlen(t)); return; }
            if (players < 1) players = 1;
            if (players > MAXLOCAL) players = MAXLOCAL;
            c = &C[i]; memset(c, 0, sizeof *c);
            int got = remotes_net_take(players, c->slots);
            if (!got) { const char *t = "No free player slots"; send_to(from, NM_REJECT, (const uint8_t *)t, (int)strlen(t)); return; }
            c->active = 1; c->addr = *from; c->players = got;
            snprintf(c->name, sizeof c->name, "%s", name[0] ? name : "Player");
            for (int k = 0; k < got; k++) {                          /* their profiles, into the game for their slots */
                memcpy(c->tag[k], tags[k], 5); c->pref[k] = prefs[k];
                if (!session_on()) chars_set_slot_profile(c->slots[k], tags[k], prefs[k] < chars_count() ? prefs[k] : -1);
            }
            set_status("%s joined (player slot%s %d%s%s)", c->name, got > 1 ? "s" : "", c->slots[0] + 1, got > 1 ? "..." : "", "");
        }
        c->last_heard = ms();
        uint8_t b[16]; wbuf w = {b, 0, sizeof b};
        w8(&w, (uint32_t)(c - C)); w8(&w, (uint32_t)c->players);
        for (int k = 0; k < MAXLOCAL; k++) w8(&w, k < c->players ? (uint32_t)c->slots[k] : 0xFF);
        w32(&w, (uint32_t)g_frames_presented); w32(&w, nonce);
        send_to(from, NM_WELCOME, b, w.n);
        return;
    }
    if (!c) return;
    c->last_heard = ms();
    if (type == NM_GETCHUNK || type == NM_READY) { host_lobby_packet(c, type, r); return; }
    if (type == NM_INPUT) {
        uint32_t newest = r32(r); int count = (int)r8(r), players = (int)r8(r);
        if (players > MAXLOCAL) players = MAXLOCAL;
        for (int k = 0; k < count && !r->bad; k++) {
            uint32_t fr = newest - (uint32_t)k;
            inframe_t *f = &c->ring[fr % HIST];
            uint32_t bb[MAXLOCAL] = {0};
            for (int p = 0; p < players; p++) bb[p] = r32(r);
            if (r->bad) break;
            if (!f->have || f->frame != fr) { f->frame = fr; f->have = 1; memcpy(f->b, bb, sizeof bb); }
        }
        if (!c->started || (int32_t)(newest - c->newest) > 0) c->newest = newest;
        if (!c->started) { c->started = 1; c->next = newest - 2; }   /* (a 2-frame jitter buffer) */
    } else if (type == NM_PING) {
        uint64_t t = r64(r); uint8_t b[8]; wbuf w = {b, 0, 8}; w64(&w, t); send_to(from, NM_PONG, b, w.n);
    } else if (type == NM_PONG) {
        uint64_t t = r64(r); note_ping(c, (int)(ms() - t));
    } else if (type == NM_BYE) {
        drop_client(c, "left");
    }
}
/* one frame of the host: play each client's next input, tell everyone the state */
static void host_frame(void) {
    uint64_t now = ms();
    for (int i = 0; i < MAXCL; i++) {
        client_t *c = &C[i];
        if (!c->active) continue;
        if (now - c->last_heard > 5000) { drop_client(c, "no answer for 5 s"); continue; }
        if (!c->started) continue;
        if ((int32_t)(c->newest - c->next) > 12) c->next = c->newest - 2;        /* far behind: catch up */
        inframe_t *f = &c->ring[c->next % HIST];
        if ((int32_t)(c->newest - c->next) >= 0 && f->have && f->frame == c->next) {
            memcpy(c->held, f->b, sizeof c->held);
            c->next++;
        }                                                                       /* (not here yet: the last buttons again) */
    }
    if (g_frames_presented % 2) return;
    /* STATE (with the lobby) to each client */
    for (int i = 0; i < MAXCL; i++) {
        client_t *c = &C[i];
        if (!c->active) continue;
        uint8_t b[512]; wbuf w = {b, 0, sizeof b};
        w32(&w, (uint32_t)g_frames_presented); w32(&w, c->next - 1);
        int n = 1; for (int j = 0; j < MAXCL; j++) if (C[j].active) n += C[j].players;
        w8(&w, (uint32_t)n);
        w8(&w, 0); w8(&w, 0); w16(&w, 0); wstr(&w, my_name(), 16);           /* the host: slot 1 */
        for (int j = 0; j < MAXCL; j++) if (C[j].active) for (int p = 0; p < C[j].players; p++) {
            w8(&w, (uint32_t)C[j].slots[p]); w8(&w, 1); w16(&w, (uint32_t)C[j].ping); wstr(&w, C[j].name, 16);
        }
        send_to(&c->addr, NM_STATE, b, w.n);
        if (g_frames_presented % 60 == 0) { uint8_t pb[8]; wbuf pw = {pb, 0, 8}; w64(&pw, now); send_to(&c->addr, NM_PING, pb, pw.n); }
    }
}

/* ---- client ---- */
static uint32_t my_nonce;                    /* (this join's HELLO nonce: hello_nonce) */
int net_join(const char *addr_text) {
    net_leave();
    char host[128]; int p = 41790;
    snprintf(host, sizeof host, "%s", addr_text);
    char *colon = strrchr(host, ':'); if (colon) { *colon = 0; p = atoi(colon + 1); }
    if (!host[0]) { set_status("Type the host's address (like 203.0.113.5)"); return 0; }
    memset(&host_addr, 0, sizeof host_addr);
    host_addr.sin_family = AF_INET; host_addr.sin_port = htons((uint16_t)p);
    if (inet_pton(AF_INET, host, &host_addr.sin_addr) != 1) {
#ifdef _WIN32
        static int wsa; if (!wsa) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); wsa = 1; }
#endif
        struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, NULL, &hints, &res) || !res) { set_status("Can't find %s", host); return 0; }
        host_addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr; freeaddrinfo(res);
    }
    if (!sock_open(0)) return 0;
    my_players = remotes_local_players(); if (my_players < 1) my_players = 1; if (my_players > MAXLOCAL) my_players = MAXLOCAL;
    role = NET_JOINING; hello_t0 = ms(); hello_last = 0; nlobby = 0; acked_frame = 0;
    my_nonce = (uint32_t)(rt_now_ns() * 2654435761ull) ^ (uint32_t)(uintptr_t)&host_addr ^ (uint32_t)rand(); if (!my_nonce) my_nonce = 1;
    set_status("Connecting to %s:%d...", host, p);
    return 1;
}
static void addr_str_of(const struct sockaddr_in *a, char *s, size_t n) {
    uint32_t ip = ntohl(a->sin_addr.s_addr);
    snprintf(s, n, "%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, ntohs(a->sin_port));
}
static void client_packet(const struct sockaddr_in *from, int type, rbuf *r) {
    if (!same_addr(from, &host_addr)) {
        char a[32], b[32]; addr_str_of(from, a, sizeof a); addr_str_of(&host_addr, b, sizeof b);
        if (type == NM_WELCOME && role == NET_JOINING && r->n >= 4 && my_nonce &&
            (uint32_t)(r->p[r->n - 4] | r->p[r->n - 3] << 8 | r->p[r->n - 2] << 16 | (uint32_t)r->p[r->n - 1] << 24) == my_nonce) {
            rt_log("net", "the host answered from %s (joined %s): talking to it there", a, b);
            host_addr = *from;
        } else {
            static uint32_t warned_ip; static uint16_t warned_port;
            if (from->sin_addr.s_addr != warned_ip || from->sin_port != warned_port) {
                warned_ip = from->sin_addr.s_addr; warned_port = from->sin_port;
                rt_log("net", "ignored a packet (type %d) from %s - the host is %s", type, a, b);
            }
            return;
        }
    }
    host_heard = ms();
    if (type == NM_WELCOME && role == NET_JOINING) {
        my_id = (int)r8(r); int n = (int)r8(r);
        for (int k = 0; k < MAXLOCAL; k++) my_slots[k] = (int)r8(r);
        r32(r);
        if (r->bad) return;
        my_players = n;
        role = NET_CLIENT;
        set_status("Connected - you are player %d%s", my_slots[0] + 1, n > 1 ? " (and more)" : "");
    } else if (type == NM_REJECT) {
        char t[100]; int n = r->n < 99 ? r->n : 99; memcpy(t, r->p, n); t[n] = 0;
        set_status("The host said no: %s", t);
        sock_close(); role = NET_OFF;
    } else if (type == NM_STATE && role == NET_CLIENT) {
        r32(r); acked_frame = r32(r); int n = (int)r8(r); if (n > 8) n = 8;
        int k; for (k = 0; k < n && !r->bad; k++) { lobby[k].slot = (int)r8(r); lobby[k].kind = (int)r8(r); lobby[k].ping = (int)r16(r); rstr(r, lobby[k].name, 16); }
        if (!r->bad) nlobby = k;
    } else if (type == NM_PING) {
        uint64_t t = r64(r); uint8_t b[8]; wbuf w = {b, 0, 8}; w64(&w, t); send_to(&host_addr, NM_PONG, b, w.n);
    } else if (type == NM_PONG) {
        my_ping = (int)(ms() - r64(r));
    } else if (type == NM_BYE) {
        set_status("The host ended the game");
        sock_close(); role = NET_OFF;
    } else if (type == NM_OFFER || type == NM_CHUNK || type == 40 /* NM_SNAP */) client_lobby_packet(type, r);
}
/* testing: RT_NET_FAKE_INPUT="frame:hex:frames,..." - the client presses those buttons */
static uint32_t fake_input(void) {
    static const char *spec; static int init;
    if (!init) { init = 1; spec = getenv("RT_NET_FAKE_INPUT"); }
    if (!spec) return 0;
    uint32_t b = 0; const char *p = spec;
    while (*p) {
        long at = strtol(p, (char **)&p, 10); if (*p != ':') break; p++;
        uint32_t m = (uint32_t)strtoul(p, (char **)&p, 16); long dur = 10;
        if (*p == ':') { p++; dur = strtol(p, (char **)&p, 10); }
        if ((long)g_frames_presented >= at && (long)g_frames_presented < at + dur) b |= m;
        if (*p == ',') p++; else break;
    }
    return b;
}
static void client_frame(void) {
    uint64_t now = ms();
    if (role == NET_JOINING) {
        if (now - hello_t0 > 10000) { set_status("No answer from the host (is its port forwarded?)"); sock_close(); role = NET_OFF; return; }
        if (now - hello_last >= 500) {
            hello_last = now;
            uint8_t b[64]; wbuf w = {b, 0, sizeof b};
            wstr(&w, my_name(), 16); w8(&w, (uint32_t)my_players); w32(&w, version_code(rt_version())); w32(&w, my_nonce);
            for (int k = 0; k < my_players && k < MAXLOCAL; k++) { char t[5]; int pref; prof_local_player(k, t, &pref); wstr(&w, t, 4); w8(&w, pref < 0 ? 0xFF : (uint32_t)pref); }
            send_to(&host_addr, NM_HELLO, b, w.n);
        }
        return;
    }
    if (now - host_heard > 5000) { set_status("Lost the host (no answer for 5 s)"); sock_close(); role = NET_OFF; return; }
    uint32_t fr = (uint32_t)g_frames_presented;
    for (int p = 0; p < MAXLOCAL; p++) sent_frames[fr % HIST][p] = p < my_players ? remotes_local_buttons(p) | (p == 0 ? fake_input() : 0) : 0;
    uint8_t b[300]; wbuf w = {b, 0, sizeof b};
    int count = 8; if ((uint32_t)count > fr) count = (int)fr;
    w32(&w, fr); w8(&w, (uint32_t)count); w8(&w, (uint32_t)my_players);
    for (int k = 0; k < count; k++) for (int p = 0; p < my_players; p++) w32(&w, sent_frames[(fr - (uint32_t)k) % HIST][p]);
    send_to(&host_addr, NM_INPUT, b, w.n);
    if (fr % 60 == 30) { uint8_t pb[8]; wbuf pw = {pb, 0, 8}; w64(&pw, now); send_to(&host_addr, NM_PING, pb, pw.n); }
}

/* ---- every update ---- */
void net_update(void) {
    static int env_done;
    lag_flush();
    if (session_on()) { session_poll(); return; }
    if (!env_done) {
        env_done = 1;
        const char *h = getenv("RT_NET_HOST"), *j = getenv("RT_NET_JOIN");
        if (h) net_host(atoi(h) ? atoi(h) : 41790);
        else if (j) net_join(j);
    }
    if (role == NET_OFF || sk == BAD_SOCK) return;
    for (int n = 0; n < 256; n++) {                                           /* everything that came in */
        uint8_t buf[1500]; struct sockaddr_in from; socklen_t fl = sizeof from;
        int got = (int)recvfrom(sk, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (got <= 0) break;
        if (got < 8) continue;
        uint32_t m = buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24;
        int len = buf[6] | buf[7] << 8;
        if (m != NET_MAGIC || buf[5] != NET_VER || len > got - 8) continue;
        rbuf r = {buf + 8, len, 0, 0};
        if (role == NET_HOST) host_packet(&from, buf[4], &r);
        else client_packet(&from, buf[4], &r);
        if (sk == BAD_SOCK) return;
    }
    static uint64_t last_frame = (uint64_t)-1;
    if (g_frames_presented == last_frame) return;                             /* (once per frame) */
    last_frame = g_frames_presented;
    if (role == NET_HOST) {
        test_start();
        host_frame(); host_offer_frame();
    } else { client_frame(); client_lobby_frame(); }
}

void net_leave(void) {
    if (role == NET_HOST) {
        for (int i = 0; i < MAXCL; i++) if (C[i].active) { send_to(&C[i].addr, NM_BYE, NULL, 0); drop_client(&C[i], "game closed"); }
        set_status("Stopped hosting");
    } else if (role == NET_CLIENT || role == NET_JOINING) {
        send_to(&host_addr, NM_BYE, NULL, 0);
        set_status("Left the game");
    }
    sock_close(); role = NET_OFF;
}

/* the buttons of an online player in one of the host's slots */
uint32_t net_slot_buttons(int slot) {
    if (role != NET_HOST) return 0;
    for (int i = 0; i < MAXCL; i++) if (C[i].active) for (int p = 0; p < C[i].players; p++) if (C[i].slots[p] == slot) return C[i].held[p];
    return 0;
}
const char *net_slot_name(int slot) {
    for (int i = 0; i < MAXCL; i++) if (C[i].active) for (int p = 0; p < C[i].players; p++) if (C[i].slots[p] == slot) return C[i].name;
    return "Online";
}
int net_slot_ping(int slot) {
    for (int i = 0; i < MAXCL; i++) if (C[i].active) for (int p = 0; p < C[i].players; p++) if (C[i].slots[p] == slot) return C[i].ping;
    return -1;
}
int net_role(void) { return role; }
const char *net_status(void) { return status; }
int net_port(void) { return port; }
/* the people in the game, for the Online screen: name, player slot, ping (-1: none), 0 = none left */
int net_lobby(int i, char *name, int n, int *slot, int *ping) {
    if (role == NET_HOST) {
        if (i == 0) { snprintf(name, n, "%s (you, host)", my_name()); *slot = 0; *ping = -1; return 1; }
        int k = 1;
        for (int j = 0; j < MAXCL; j++) if (C[j].active) for (int p = 0; p < C[j].players; p++, k++)
            if (k == i) { snprintf(name, n, "%s", C[j].name); *slot = C[j].slots[p]; *ping = C[j].ping; return 1; }
        return 0;
    }
    if (role == NET_CLIENT && i < nlobby) {
        int mine = 0; for (int p = 0; p < my_players; p++) if (my_slots[p] == lobby[i].slot) mine = 1;
        snprintf(name, n, "%s%s", lobby[i].name, mine ? " (you)" : lobby[i].kind == 0 ? " (host)" : "");
        *slot = lobby[i].slot; *ping = mine ? my_ping : lobby[i].ping; return 1;
    }
    return 0;
}

/* ======================================================================== online game
 * Lockstep on the deterministic emulation (rt_det.c, rt_core.c virtual clock), started from the
 * host's running game: no restart, nobody goes back to the boot screens.
 *
 * Starting (the host picks Start the online game):
 *   OFFER    h->c  session id, input delay, clock, player map, the files the game uses (the
 *                  host's save files, SYSCONF, setting.txt, its settings.ini): sizes + hashes
 *   GETCHUNK c->h  blob, offset          CHUNK h->c  blob, offset, bytes (a blob is a file, the
 *                  page hash table, or one client's pages; many requests in flight)
 *   READY    c->h  the files are in <game>/online/p<n>/
 *   Everyone ready: the host switches to deterministic mode and, at the next video field, takes a
 *   save state of its game (rt_state.c: S0). From then on it is in the online game.
 *   SNAP     h->c  session id, field, size of the page hash table, the state's region table
 *   A client freezes its game, switches to deterministic mode and keeps a save state of its own
 *   game (to go back to afterwards). It fetches the hash of every 4 KB page of S0, compares them
 *   with its own memory at the same addresses (both run the same game: most pages are the same)
 *   WANT     c->h  a bitmap of the pages it needs (in parts, again until answered)
 *   PAGES    h->c  the size and hash of a blob with those pages (each compressed)
 *   It fetches that blob, puts S0 together (its own pages + the host's), checks every page
 *   against the hash table and loads it: it continues exactly where the host took S0.
 * In the game: player slots come from the map (host's players first, then each client's). At
 * every video field F each PC samples its players for field F + delay and sends them (SINPUT,
 * the last 8 again each time) to the host; the host publishes a field once it has everyone's
 * buttons for it (SFRAME, the last 8 again). A PC runs field F only when it has it (det_field
 * waits). Every 120 fields each PC sends a hash of its game (SHASH); a difference shows OUT OF
 * SYNC everywhere (SDESYNC). The game's NAND is <game>/online/<name>/nand (the host's save:
 * nobody's own save is touched), and its game settings are the host's (each PC keeps its own
 * display / sound / network settings).
 * Ending (Leave, a BYE, nobody heard from for 10 s): everyone loads the save state of their own
 * game from before and plays on from there (the host: S0). */
#define XXH_INLINE_ALL
#include "xxhash.h"
enum { NM_SNAP = 40, NM_WANT, NM_PAGES };
#define SMAXFILES 12
#define SRING 512
#define MAXPEER 8
#define CHUNK 1200
#define PAGE 4096
#define BLOB_HASHES 100
#define BLOB_PAGES 200
#define WIN 320                               /* chunk requests in flight (384 KB: ~2.5 MB/s at 150 ms) */
typedef struct { char path[96]; uint32_t size; uint64_t hash; uint8_t *data; } sfile_t;
/* fetching a blob from the host, with many requests in flight */
typedef struct { int blob; uint32_t size, nch, ngot, cursor; uint8_t *data, *got; uint32_t fl_ch[WIN]; uint64_t fl_t[WIN]; int nfl; } fetch_t;
enum { LR_NONE, LR_JOIN, LR_BACK };
static struct {
    int phase;                                /* see the P_ values */
    uint32_t id; int delay; uint32_t rtc;
    sfile_t f[SMAXFILES]; int nf;
    int map_peer[8], map_local[8];            /* slot -> PC (-1 none, 0 host, 1.. client i - 1), its local player */
    int peer_ready[MAXCL];
    /* the game */
    int on, host, me, npeers, nlocal;
    uint32_t base;                            /* the field the game started at */
    uint64_t peer_heard[MAXPEER]; int peer_started[MAXPEER];
    uint32_t pin[MAXPEER][SRING][MAXLOCAL]; uint32_t pin_fr[MAXPEER][SRING]; uint8_t pin_have[MAXPEER][SRING];   /* host: each PC's buttons per field */
    uint32_t fr_btn[SRING][8]; uint32_t fr_fr[SRING]; uint8_t fr_have[SRING];                                   /* published fields */
    uint32_t pub_next;
    uint32_t my_in[SRING][MAXLOCAL];
    uint32_t cur_btn[8];
    uint64_t hashes[64]; uint32_t hash_fr[64];
    uint64_t peer_hash[MAXPEER][64]; uint32_t peer_hash_fr[MAXPEER][64];
    int desync; uint32_t desync_fr;
    uint64_t host_heard;
    /* the start (host) */
    rt_snap *s0;                              /* host: the game when it started = its own game to go back to */
    uint8_t *hashes_blob; uint32_t hashes_size; uint32_t npages;
    uint8_t *want[MAXCL]; uint8_t *want_parts[MAXCL]; uint32_t want_nparts[MAXCL];
    uint8_t *pages_blob[MAXCL]; uint32_t pages_size[MAXCL]; uint64_t pages_hash[MAXCL];
    uint64_t snap_sent;
    int host_starting;
    rt_snap *offer, *sre;                     /* host: the state being sent (S0 at the start, a newer one for a rejoin) */
    uint32_t offer_field;
    int snap_for[MAXCL];                      /* host: this client is to get the offered state */
    int rejoin_pending;                       /* host: a rejoining client is ready: take a state at the next field */
    int rejoin_offer[MAXCL]; uint64_t offer_sent;
    int snap_wait[MAXCL];                     /* host: ready for a state; it gets the next one taken */
    /* players who dropped out: the game goes on, their buttons are none until they're back */
    int absent[MAXPEER], gone[MAXPEER];
    uint64_t stall_since;
    uint32_t (*hist)[8]; uint32_t hist_n, hist_cap;   /* host: every published field since base (rejoin / catch up) */
    uint32_t newest_seen; uint64_t last_req, last_input_sent;
    uint8_t last_sinput[700]; int last_sinput_n;
    /* rollback (rb_field): every PC runs each field at once with the buttons it has, guessing the
       others' (their last known), and goes back when a guess was wrong */
    int rb;                                   /* this PC runs the online game with rollback */
    int rb_session;                           /* the online game is a rollback one (a PC that can't: the host's fields only) */
    uint32_t conf[MAXPEER];                   /* each PC's buttons are known up to this field */
    uint32_t used[SRING][8]; uint32_t used_fr[SRING];   /* what each field ran with */
    uint32_t mis;                             /* earliest field that ran with a wrong guess (NOMIS: none) */
    uint32_t decided;                         /* the field whose buttons were decided last (this run) */
    uint32_t final_to;                        /* client: the host published every field up to here */
    uint64_t fast_t0; uint32_t fast_f0;       /* client: catching up since */
    uint32_t resim_to; int in_resim;          /* fields before resim_to run again silently */
    uint32_t peer_frame[MAXPEER]; uint64_t peer_frame_t[MAXPEER];   /* each PC's field as last heard (time sync) */
    uint32_t synth_to[MAXPEER];               /* host: fields it decided for an away / gone PC (no buttons) */
    uint32_t my_ack;                          /* client: the host has our buttons up to here */
    uint64_t stall_t0;
    uint64_t rh[64]; uint32_t rh_fr[64]; uint8_t rh_done[64];   /* state checks: computed, sent once final */
    uint64_t st_rollbacks, st_rb_fields, st_rb_ns, st_stall_ms, st_sync_ms;   /* (statistics) */
    /* the direct path (a client): fields put together from everyone's buttons as they come in */
    uint32_t back_from[MAXPEER];              /* host: the first field it uses each PC's own buttons for */
    uint32_t my_from; int my_from_known;      /* client: ... ours, as the host last said (until then: its fields only) */
    uint32_t my_from_stamp;
    uint32_t ran_btn[SRING][8]; uint32_t ran_fr[SRING]; uint8_t ran_have[SRING];   /* client: fields run on the direct path (checked against the host's) */
    int resync; uint64_t resync_sent; int nresync; uint64_t resync_t0;            /* client: getting the host's game again */
    uint8_t epoch, peer_epoch[MAXPEER];       /* how many times it got the game again (SINPUT carries it: older ones are stale) */
    int direct_fields, host_fields;           /* (statistics) */
    /* a client */
    rt_snap *own;                             /* its own game, to go back to */
    fetch_t fe; int file_i;
} SS;
enum { P_NONE, P_OFFER, P_SNAP_PENDING, P_FILES, P_READY, P_LOADING };
static int load_reason;
static char end_msg[160];
static uint64_t notice_t;
const char *net_notice(void) { return notice_t && ms() - notice_t < 7000 ? end_msg : NULL; }   /* (shown over the game after an online game ends) */
int g_resim;                                          /* rt.h: fields run again after a rollback, or catching up - nothing drawn or heard */
int g_det_fast;                                       /* rt_core.c det_pace: catching up with the others, run unpaced */

static char sess_name[16];
static const char *session_dir(char *out, size_t n) { snprintf(out, n, "%s/online/%s", g_root, sess_name); return out; }
static void name_for(int host, int me) { if (host) snprintf(sess_name, sizeof sess_name, "host"); else snprintf(sess_name, sizeof sess_name, "p%d", me); }
static void mkdir_p(const char *path) {
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/' || *p == '\\') { char c = *p; *p = 0;
#ifdef _WIN32
        CreateDirectoryA(tmp, NULL);
#else
        mkdir(tmp, 0755);
#endif
        *p = c; }
}
static uint8_t *read_file(const char *p, uint32_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(l > 0 ? l : 1); if (l > 0 && fread(d, 1, l, f) != (size_t)l) l = 0; fclose(f);
    *n = (uint32_t)l; return d;
}
static int write_file(const char *p, const uint8_t *d, uint32_t n) {
    mkdir_p(p); FILE *f = fopen(p, "wb"); if (!f) return 0; fwrite(d, 1, n, f); fclose(f); return 1;
}
static uint64_t fnv64(const uint8_t *d, uint32_t n) { uint64_t h = 1469598103934665603ull; for (uint32_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; } return h; }

/* ---- a small LZ77 for memory pages (token: literals count | match length, LZ4-like) ---- */
static int lz_pack(const uint8_t *src, int n, uint8_t *dst, int cap) {
    uint16_t ht[1 << 12]; memset(ht, 0xFF, sizeof ht);
    int ip = 0, op = 0, lit = 0;
#define LZ_H(p) ((uint32_t)((src[p] | src[(p) + 1] << 8 | src[(p) + 2] << 16 | (uint32_t)src[(p) + 3] << 24) * 2654435761u) >> 20)
    while (ip + 4 <= n) {
        uint32_t h = LZ_H(ip); int cand = ht[h] == 0xFFFF ? -1 : ht[h]; ht[h] = (uint16_t)ip;
        if (cand >= 0 && ip - cand < 65535 && !memcmp(src + cand, src + ip, 4)) {
            int ml = 4; while (ip + ml < n && src[cand + ml] == src[ip + ml]) ml++;
            /* emit: token, literal length ext, literals, offset, match length ext */
            if (op + 1 + lit / 255 + 1 + lit + 2 + ml / 255 + 1 > cap) return -1;
            int tl = lit < 15 ? lit : 15, tm = ml - 4 < 15 ? ml - 4 : 15;
            dst[op++] = (uint8_t)(tl << 4 | tm);
            if (tl == 15) { int r = lit - 15; while (r >= 255) { dst[op++] = 255; r -= 255; } dst[op++] = (uint8_t)r; }
            memcpy(dst + op, src + ip - lit, lit); op += lit;
            int off = ip - cand; dst[op++] = (uint8_t)off; dst[op++] = (uint8_t)(off >> 8);
            if (tm == 15) { int r = ml - 4 - 15; while (r >= 255) { dst[op++] = 255; r -= 255; } dst[op++] = (uint8_t)r; }
            ip += ml; lit = 0;
        } else { ip++; lit++; }
    }
    lit += n - ip;
    if (op + 1 + lit / 255 + 1 + lit > cap) return -1;
    int tl = lit < 15 ? lit : 15;
    dst[op++] = (uint8_t)(tl << 4);
    if (tl == 15) { int r = lit - 15; while (r >= 255) { dst[op++] = 255; r -= 255; } dst[op++] = (uint8_t)r; }
    memcpy(dst + op, src + n - lit, lit); op += lit;
    return op;
#undef LZ_H
}
static int lz_unpack(const uint8_t *src, int n, uint8_t *dst, int cap) {
    int ip = 0, op = 0;
    while (ip < n) {
        int t = src[ip++], lit = t >> 4, ml = t & 15;
        if (lit == 15) { int b; do { if (ip >= n) return -1; b = src[ip++]; lit += b; } while (b == 255); }
        if (ip + lit > n || op + lit > cap) return -1;
        memcpy(dst + op, src + ip, lit); ip += lit; op += lit;
        if (ip >= n) break;                           /* (the last token has no match) */
        if (ip + 2 > n) return -1;
        int off = src[ip] | src[ip + 1] << 8; ip += 2;
        if (ml == 15) { int b; do { if (ip >= n) return -1; b = src[ip++]; ml += b; } while (b == 255); }
        ml += 4;
        if (off <= 0 || off > op || op + ml > cap) return -1;
        for (int k = 0; k < ml; k++) { dst[op] = dst[op - off]; op++; }
    }
    return op;
}

/* ---- the state's pages: the region table at the front of a state, 4 KB pages of each region ---- */
typedef struct { uint64_t addr, size; } sregion;
typedef struct { uint32_t magic, build, nreg, cur_slot; uint64_t total; sregion reg[54]; } shdr;   /* (rt_state.c snap_hdr) */
static uint32_t region_pages(const shdr *h, uint32_t i) { return (uint32_t)((h->reg[i].size + PAGE - 1) / PAGE); }
static uint32_t total_pages(const shdr *h) { uint32_t n = 0; for (uint32_t i = 0; i < h->nreg && i < 54; i++) n += region_pages(h, i); return n; }
/* page p of a state: its region, address, length, and offset in the state's bytes */
static int page_info(const shdr *h, uint32_t p, uint32_t *reg, uint64_t *addr, uint32_t *len, uint64_t *off) {
    uint64_t o = sizeof(shdr);
    for (uint32_t i = 0; i < h->nreg && i < 54; i++) {
        uint32_t np = region_pages(h, i);
        if (p < np) {
            *reg = i; *addr = h->reg[i].addr + (uint64_t)p * PAGE; *off = o + (uint64_t)p * PAGE;
            uint64_t rest = h->reg[i].size - (uint64_t)p * PAGE; *len = rest < PAGE ? (uint32_t)rest : PAGE;
            return 1;
        }
        p -= np; o += h->reg[i].size;
    }
    return 0;
}

/* the files a game uses: the host's save, console settings, and its settings.ini */
static const char *session_files[] = {
    "nand/title/00010000/534d4e45/data/nsmbw-save.json", "nand/title/00010000/534d4e45/data/mkwcat-nsmbw.json",
    "nand/title/00010000/534d4e45/data/mkwcat-nsmbw-old.json", "nand/title/00010000/534d4e45/data/wiimj2d.sav",
    "nand/title/00010000/534d4e45/data/banner.bin", "nand/shared2/sys/SYSCONF", "nand/title/00000001/00000002/data/setting.txt",
    "settings.ini", NULL };
static void write_session_files(void) {
    char d[1200]; session_dir(d, sizeof d);
    for (int i = 0; i < SS.nf; i++) { char p[1400]; snprintf(p, sizeof p, "%s/%s", d, SS.f[i].path); write_file(p, SS.f[i].data, SS.f[i].size); }
}

/* ---- serving blobs (host) ---- */
static const uint8_t *blob_of(int blob, int ci, uint32_t *size) {
    if (blob >= 0 && blob < SS.nf) { *size = SS.f[blob].size; return SS.f[blob].data; }
    if (blob == BLOB_HASHES && SS.hashes_blob) { *size = SS.hashes_size; return SS.hashes_blob; }
    if (blob == BLOB_PAGES && ci >= 0 && SS.pages_blob[ci]) { *size = SS.pages_size[ci]; return SS.pages_blob[ci]; }
    return NULL;
}
static void serve_chunk(client_t *c, rbuf *r) {
    int blob = (int)r16(r); uint32_t off = r32(r), size;
    if (r->bad) return;
    const uint8_t *d = blob_of(blob, (int)(c - C), &size);
    if (!d || off >= size) return;
    uint32_t n = size - off; if (n > CHUNK) n = CHUNK;
    uint8_t b[CHUNK + 16]; wbuf w = {b, 0, sizeof b}; w16(&w, (uint32_t)blob); w32(&w, off); w16(&w, n);
    memcpy(b + w.n, d + off, n); w.n += (int)n;
    send_to(&c->addr, NM_CHUNK, b, w.n);
}
/* ---- fetching blobs (client) ---- */
static void fetch_start(fetch_t *f, int blob, uint32_t size) {
    free(f->data); free(f->got); memset(f, 0, sizeof *f);
    f->blob = blob; f->size = size; f->nch = (size + CHUNK - 1) / CHUNK;
    f->data = calloc(1, size ? size : 1); f->got = calloc(1, f->nch ? f->nch : 1);
}
static int fetch_done(const fetch_t *f) { return f->data && f->ngot == f->nch; }
static void fetch_chunk(fetch_t *f, rbuf *r) {
    int blob = (int)r16(r); uint32_t off = r32(r), n = r16(r);
    if (r->bad || !f->data || blob != f->blob || off % CHUNK || off >= f->size || off + n > f->size || r->i + (int)n > r->n) return;
    uint32_t ch = off / CHUNK;
    if (f->got[ch]) return;
    memcpy(f->data + off, r->p + r->i, n); f->got[ch] = 1; f->ngot++;
}
static void fetch_step(fetch_t *f) {                  /* keep WIN requests in flight, again after 300 ms */
    if (!f->data || fetch_done(f)) return;
    uint64_t now = ms();
    for (int i = 0; i < f->nfl; ) {
        if (f->got[f->fl_ch[i]]) { f->fl_ch[i] = f->fl_ch[--f->nfl]; f->fl_t[i] = f->fl_t[f->nfl]; continue; }
        if (now - f->fl_t[i] > 300) {
            uint8_t b[8]; wbuf w = {b, 0, 8}; w16(&w, (uint32_t)f->blob); w32(&w, f->fl_ch[i] * CHUNK); send_to(&host_addr, NM_GETCHUNK, b, w.n);
            f->fl_t[i] = now;
        }
        i++;
    }
    for (uint32_t scanned = 0; f->nfl < WIN && scanned < f->nch; scanned++) {
        uint32_t ch = f->cursor; f->cursor = (f->cursor + 1) % f->nch;
        if (f->got[ch]) continue;
        int busy = 0; for (int i = 0; i < f->nfl; i++) if (f->fl_ch[i] == ch) busy = 1;
        if (busy) continue;
        uint8_t b[8]; wbuf w = {b, 0, 8}; w16(&w, (uint32_t)f->blob); w32(&w, ch * CHUNK); send_to(&host_addr, NM_GETCHUNK, b, w.n);
        f->fl_ch[f->nfl] = ch; f->fl_t[f->nfl] = now; f->nfl++;
    }
}

/* ---- the lobby: offer, files, ready ---- */
static int auto_delay(void);
int net_start_session(void) {
    if (role != NET_HOST || SS.on) return 0;
    if (!rt_state_ok()) { set_status("Online games need save states, which aren't available on this PC (see the log)"); return 0; }
    int n = 0; for (int i = 0; i < MAXCL; i++) if (C[i].active) n++;
    if (!n) { set_status("Nobody has joined yet"); return 0; }
    for (int i = 0; i < SS.nf; i++) free(SS.f[i].data);
    memset(&SS, 0, sizeof SS);
    SS.id = (uint32_t)(rt_now_ns() ^ (rt_now_ns() >> 29)) | 1;
    SS.rb_session = SS.rb = rb_supported() && rt_setting("net_rollback", 1);
    if (SS.rb) { int d = rt_setting("net_input_delay", 0); SS.delay = d < 0 ? 0 : d > 8 ? 8 : d; rt_log("net", "rollback, input delay %d", SS.delay); }
    else SS.delay = auto_delay();
    SS.rtc = (uint32_t)(time(NULL) - 946684800);       /* (the Wii counts from 2000) */
    /* the player map: the slots everyone already has in the host's game (its own players', and
       the joined players' online slots), so the character select keeps its players */
    for (int s = 0; s < 8; s++) SS.map_peer[s] = -1;
    int hl = remotes_local_players(); if (hl < 1) hl = 1;
    for (int k = 0; k < hl; k++) { int s = remotes_local_slot(k); if (s < 0 && k == 0) s = 0; if (s >= 0 && s < 8 && SS.map_peer[s] < 0) { SS.map_peer[s] = 0; SS.map_local[s] = k; } }
    for (int i = 0; i < MAXCL; i++) if (C[i].active) for (int k = 0; k < C[i].players; k++) { int s = C[i].slots[k]; if (s >= 0 && s < 8 && SS.map_peer[s] < 0) { SS.map_peer[s] = i + 1; SS.map_local[s] = k; } }
    for (int i = 0; session_files[i] && SS.nf < SMAXFILES; i++) {
        char p[1300]; snprintf(p, sizeof p, "%s/%s", g_root, session_files[i]);
        uint32_t sz; uint8_t *d = read_file(p, &sz); if (!d) continue;
        sfile_t *f = &SS.f[SS.nf++]; snprintf(f->path, sizeof f->path, "%s", session_files[i]); f->size = sz; f->data = d; f->hash = fnv64(d, sz);
    }
    SS.phase = P_OFFER; SS.host_starting = 1;
    set_status("Starting: sending the save and settings to everyone...");
    return 1;
}
/* The input delay: a PC runs field F once it has everyone's buttons for F, and each PC sends its
   buttons for F + delay at field F - so the delay must cover the trip from each PC to each other
   one (host <-> client: half a round trip; client <-> client: through the host, half of each
   one's). settings.ini net_input_delay: 0 = from the measured pings (the worst of the last few),
   1-15 = that many fields. */
#define MAXDELAY 15
static int auto_delay(void) {
    int set = rt_setting("net_input_delay", 0);
    if (set > 0) return set > MAXDELAY ? MAXDELAY : set;
    int r1 = 0, r2 = 0;                               /* the two worst round trips */
    for (int i = 0; i < MAXCL; i++) if (C[i].active) { int p = ping_high(&C[i]); if (p > r1) { r2 = r1; r1 = p; } else if (p > r2) r2 = p; }
    int path = (r1 + r2) / 2 > r1 / 2 ? (r1 + r2) / 2 : r1 / 2;    /* ms, one way, worst pair */
    int d = (path + 8 + 16) / 17 + 1;                 /* (+8 ms jitter, +1 field: sampled at the start of a field, read at the start of one) */
    if (d < 2) d = 2;
    if (d > MAXDELAY) d = MAXDELAY;
    rt_log("net", "input delay %d fields (%d ms): the worst round trips %d / %d ms", d, d * 1000 / 60, r1, r2);
    return d;
}
static void send_offer(client_t *c) {
    uint8_t b[1400]; wbuf w = {b, 0, sizeof b};
    w32(&w, SS.id); w8(&w, (uint32_t)SS.delay); w32(&w, SS.rtc);
    for (int s = 0; s < 8; s++) { w8(&w, (uint32_t)(SS.map_peer[s] & 0xFF)); w8(&w, (uint32_t)SS.map_local[s]); }
    w8(&w, (uint32_t)SS.nf);
    for (int i = 0; i < SS.nf; i++) { w32(&w, SS.f[i].size); w64(&w, SS.f[i].hash); wstr(&w, SS.f[i].path, 95); }
    w8(&w, (uint32_t)SS.rb_session);
    send_to(&c->addr, NM_OFFER, b, w.n);
}
static void test_start(void) {         /* testing: RT_NET_START=<frame> starts the online game once someone joined */
    static long start_at = -2;
    if (start_at == -2) { const char *e = getenv("RT_NET_START"); start_at = e ? atol(e) : -1; }
    if (start_at >= 0 && (long)g_frames_presented >= start_at && SS.phase == P_NONE && !SS.on) { int any = 0; for (int i = 0; i < MAXCL; i++) any |= C[i].active; if (any) { start_at = -1; net_start_session(); } }
}
static void host_offer_frame(void) {
    if (SS.phase != P_OFFER) return;
    int all = 1;
    for (int i = 0; i < MAXCL; i++) if (C[i].active && !SS.peer_ready[i]) { all = 0; if (g_frames_presented % 15 == 0) send_offer(&C[i]); }
    if (!all) return;
    name_for(1, 0); write_session_files();
    rt_det_enable();                                  /* (the game becomes deterministic now; S0 at the next field) */
    SS.phase = P_SNAP_PENDING;
    set_status("Everyone is ready - starting");
}
static void host_lobby_packet(client_t *c, int type, rbuf *r) {
    if (type == NM_GETCHUNK) serve_chunk(c, r);
    else if (type == NM_READY && SS.phase == P_OFFER) {
        if (r32(r) == SS.id && !SS.peer_ready[c - C]) { SS.peer_ready[c - C] = 1; set_status("%s is ready", c->name); }
    }
}
static void client_load(rbuf *r, int resync);
static void request_resync(const char *why);
static void client_lobby_packet(int type, rbuf *r) {
    if (type == NM_OFFER) {
        uint32_t id = r32(r);
        if (SS.phase != P_NONE && SS.id == id) return;
        if (SS.on) return;
        for (int i = 0; i < SS.nf; i++) free(SS.f[i].data);
        memset(&SS, 0, sizeof SS);
        SS.id = id; SS.delay = (int)r8(r); SS.rtc = r32(r); if (SS.delay < 0 || SS.delay > MAXDELAY) SS.delay = 3;
        for (int s = 0; s < 8; s++) { int p = (int)r8(r); SS.map_peer[s] = p == 0xFF ? -1 : p; SS.map_local[s] = (int)r8(r); }
        SS.nf = (int)r8(r); if (SS.nf > SMAXFILES) SS.nf = SMAXFILES;
        for (int i = 0; i < SS.nf; i++) { SS.f[i].size = r32(r); SS.f[i].hash = r64(r); rstr(r, SS.f[i].path, 95); }
        if (r->bad) { SS.nf = 0; SS.phase = P_NONE; return; }
        SS.rb_session = r->i < r->n ? (int)r8(r) : 0;
        SS.rb = SS.rb_session && rb_supported();
        for (int i = 0; i < SS.nf; i++) if (strstr(SS.f[i].path, "..") || SS.f[i].path[0] == '/' || strchr(SS.f[i].path, ':') || SS.f[i].size > (1u << 20)) { SS.nf = 0; set_status("The host sent a bad file list"); return; }
        SS.phase = P_FILES; SS.file_i = 0;
        if (SS.nf) fetch_start(&SS.fe, 0, SS.f[0].size);
        set_status("The host is starting the game - getting its save and settings...");
    } else if (type == NM_CHUNK) fetch_chunk(&SS.fe, r);
    else if (type == NM_SNAP && SS.phase == P_READY) client_load(r, 0);
}
static void client_lobby_frame(void) {
    if (SS.phase != P_FILES) return;
    if (SS.file_i < SS.nf && !fetch_done(&SS.fe)) { fetch_step(&SS.fe); return; }
    if (SS.file_i < SS.nf) {                          /* one file in: check it, on to the next */
        sfile_t *f = &SS.f[SS.file_i];
        if (fnv64(SS.fe.data, f->size) != f->hash) { set_status("A file from the host arrived damaged - try again"); SS.phase = P_NONE; return; }
        f->data = malloc(f->size ? f->size : 1); memcpy(f->data, SS.fe.data, f->size);
        if (++SS.file_i < SS.nf) { fetch_start(&SS.fe, SS.file_i, SS.f[SS.file_i].size); return; }
    }
    name_for(0, my_id + 1); write_session_files();
    uint8_t b[4]; wbuf w = {b, 0, 4}; w32(&w, SS.id); send_to(&host_addr, NM_READY, b, w.n);
    SS.phase = P_READY;
    set_status("Ready - waiting for the others");
}

/* ---- the game ---- */
int net_session_det(void) { return SS.on; }
int net_online_starting(void) { return SS.phase != P_NONE && SS.host_starting; }
int net_online_players(void) { int n = 0; for (int i = 0; i < MAXCL; i++) if (role == NET_HOST && C[i].active) n++; return n; }
int session_on(void) { return SS.on; }
const char *rt_nand_root(void) {
    static char p[1200];
    if (SS.on) snprintf(p, sizeof p, "%s/online/%s/nand", g_root, sess_name); else snprintf(p, sizeof p, "%s/nand", g_root);
    return p;
}
const char *rt_session_dir(void) { static char d[1200]; if (!SS.on) return NULL; return session_dir(d, sizeof d); }
int session_slot_active(int s) { return SS.on && s >= 0 && s < 8 && SS.map_peer[s] >= 0; }
uint32_t session_slot_buttons(int s) { return SS.on && s >= 0 && s < 8 ? SS.cur_btn[s] : 0; }
uint32_t session_rtc(void) { return SS.rtc; }
void ios_state_loaded(void);
static void game_begin(void) {                        /* host and client: the game starts at this field */
    extern uint64_t g_vi_fields;
    SS.on = 1; SS.base = (uint32_t)g_vi_fields; SS.pub_next = SS.base; SS.phase = P_NONE;
    SS.npeers = 0; SS.nlocal = 0;
    for (int s = 0; s < 8; s++) if (SS.map_peer[s] >= 0) {
        if (SS.map_peer[s] + 1 > SS.npeers) SS.npeers = SS.map_peer[s] + 1;
        if (SS.map_peer[s] == SS.me && SS.map_local[s] + 1 > SS.nlocal) SS.nlocal = SS.map_local[s] + 1;
    }
    for (int i = 0; i < MAXPEER; i++) SS.peer_heard[i] = ms();
    SS.host_heard = ms();
    /* fields before the delay: nobody pressed anything (host: everyone's; everyone: our own -
       a client gets the others' for them from the host's published fields) */
    memset(SS.pin_have, 0, sizeof SS.pin_have); memset(SS.ran_have, 0, sizeof SS.ran_have);
    for (uint32_t fr = SS.base; fr < SS.base + (uint32_t)SS.delay; fr++) {
        uint32_t k = fr % SRING;
        if (SS.host) for (int pr = 0; pr < MAXPEER; pr++) { SS.pin_fr[pr][k] = fr; SS.pin_have[pr][k] = 1; memset(SS.pin[pr][k], 0, sizeof SS.pin[pr][k]); }
        memset(SS.my_in[k], 0, sizeof SS.my_in[k]);
    }
    for (int pr = 0; pr < MAXPEER; pr++) SS.back_from[pr] = SS.base;
    if (SS.rb) {                                      /* rollback: everyone's buttons known up to the delay (none) */
        for (uint32_t fr = SS.base; fr < SS.base + (uint32_t)SS.delay; fr++) {
            uint32_t k = fr % SRING;
            for (int pr = 0; pr < MAXPEER; pr++) { SS.pin_fr[pr][k] = fr; SS.pin_have[pr][k] = 1; memset(SS.pin[pr][k], 0, sizeof SS.pin[pr][k]); }
        }
        for (int pr = 0; pr < MAXPEER; pr++) { SS.conf[pr] = SS.base + (uint32_t)SS.delay - 1; SS.synth_to[pr] = SS.base + (uint32_t)SS.delay - 1; SS.peer_frame_t[pr] = 0; }
        memset(SS.used_fr, 0xFF, sizeof SS.used_fr); SS.mis = NOMIS; SS.decided = NOMIS; SS.final_to = SS.base + (uint32_t)SS.delay - 1; SS.resim_to = SS.base; SS.in_resim = 0; g_resim = 0;
        SS.my_ack = SS.base + (uint32_t)SS.delay - 1; memset(SS.rh_fr, 0, sizeof SS.rh_fr);
        if (!rb_start()) { rt_log("net", "rollback isn't possible on this PC: the host's fields only"); SS.rb = 0; }
    }
    if (SS.resync) SS.epoch++;                        /* (got the game again: the host expects the next epoch) */
    SS.my_from_known = 0; SS.my_from_stamp = 0; SS.resync = 0; SS.direct_fields = SS.host_fields = 0;
    memset(SS.absent, 0, sizeof SS.absent); memset(SS.gone, 0, sizeof SS.gone); SS.stall_since = 0;
    SS.hist_n = 0; SS.newest_seen = SS.base;
    rt_settings_reload();                             /* (the host's game settings) */
    ios_state_loaded();                               /* (open saves: in the game's NAND now) */
    rt_log("net", "online game: %s, starting at field %u, %d PCs, input delay %d", SS.host ? "host" : "client", SS.base, SS.npeers, SS.delay);
    set_status("Online game");
    plat_title_note(SS.host ? "Online (host)" : "Online");
}
/* back to our own game (a save state from before), the online game is over */
static void game_end(const char *why) {
    rt_log("net", "online game over: %s", why);
    if (sk != BAD_SOCK) for (int k = 0; k < 3; k++) {
        if (SS.host) { for (int i = 0; i < MAXCL; i++) if (C[i].active) send_to(&C[i].addr, NM_BYE, NULL, 0); }
        else send_to(&host_addr, NM_BYE, NULL, 0);
    }
    snprintf(end_msg, sizeof end_msg, "The online game ended: %s", why);
    SS.on = 0; SS.phase = P_NONE; g_det_fast = 0;
    rt_snap *back = SS.host ? SS.s0 : SS.own;
    plat_title_note(NULL);
    rb_stop(); g_resim = 0;
    if (back) { load_reason = LR_BACK; rt_snap_load(back); }
    rt_fatal("no save state to go back to after the online game");
}
static void back_home(void) {                         /* resumed in our own game */
    load_reason = LR_NONE;
    rt_settings_reload();
    set_status("%s", end_msg); notice_t = ms();
    if (SS.host) { rt_snap_free(SS.s0); SS.s0 = NULL; SS.offer = NULL; free(SS.hist); SS.hist = NULL; SS.hist_cap = SS.hist_n = 0; for (int i = 0; i < MAXCL; i++) { free(SS.pages_blob[i]); SS.pages_blob[i] = NULL; free(SS.want[i]); SS.want[i] = NULL; free(SS.want_parts[i]); SS.want_parts[i] = NULL; } free(SS.hashes_blob); SS.hashes_blob = NULL; }
    else { if (role == NET_CLIENT) send_to(&host_addr, NM_BYE, NULL, 0); }
    if (role == NET_CLIENT) { sock_close(); role = NET_OFF; }   /* (a client leaves the lobby: join again for the next game) */
    if (role == NET_HOST) for (int i = 0; i < MAXCL; i++) C[i].last_heard = ms();
}
void net_shutdown(void) {
    if (SS.on) {
        for (int k = 0; k < 3; k++) {
            if (SS.host) { for (int i = 0; i < MAXCL; i++) if (C[i].active) send_to(&C[i].addr, NM_BYE, NULL, 0); }
            else send_to(&host_addr, NM_BYE, NULL, 0);
        }
    } else if (role != NET_OFF) net_leave();
    lag_drain();
}
void net_leave_session(void) { if (SS.on) game_end("you left the online game"); }

/* rt_det.c, every video field before session_field: the host's start (S0), and where loads resume */
int net_snap_pending(void) { return SS.phase == P_SNAP_PENDING || (SS.on && SS.host && SS.rejoin_pending); }
static void host_hash_pages(void) {
    size_t n; const uint8_t *s = rt_snap_data(SS.offer, &n);
    free(SS.hashes_blob);
    const shdr *h = (const shdr *)s;
    SS.npages = total_pages(h);
    SS.hashes_size = SS.npages * 8; SS.hashes_blob = malloc(SS.hashes_size);
    for (uint32_t p = 0; p < SS.npages; p++) {
        uint32_t reg, len; uint64_t addr, off; page_info(h, p, &reg, &addr, &len, &off);
        uint64_t x = XXH3_64bits(s + off, len); memcpy(SS.hashes_blob + 8 * p, &x, 8);
    }
}
static void send_snap(void) {
    size_t n; const uint8_t *s = rt_snap_data(SS.offer, &n);
    uint8_t b[1400]; wbuf w = {b, 0, sizeof b};
    w32(&w, SS.id); w32(&w, SS.offer_field); w32(&w, SS.hashes_size);
    memcpy(b + w.n, s, sizeof(shdr)); w.n += (int)sizeof(shdr);
    for (int i = 0; i < MAXCL; i++) if (C[i].active && SS.snap_for[i] && !SS.pages_blob[i]) send_to(&C[i].addr, NM_SNAP, b, w.n);
    SS.snap_sent = ms();
}
static void offer_reset(int ci) { free(SS.pages_blob[ci]); SS.pages_blob[ci] = NULL; free(SS.want[ci]); SS.want[ci] = NULL; free(SS.want_parts[ci]); SS.want_parts[ci] = NULL; SS.want_nparts[ci] = 0; }
void net_snap_point(void) {
    extern uint64_t g_vi_fields;
    uint64_t t = rt_now_ns();
    if (SS.on && SS.host) {                           /* a player rejoins: a state of the game now */
        SS.rejoin_pending = 0;
        if (!SS.sre) SS.sre = rt_snap_new();
        int r = rt_snap_take(SS.sre);
        if (r == 1) {                                 /* (the rejoining player resumes here) */
            if (load_reason == LR_JOIN) { load_reason = LR_NONE; game_begin(); }
            else if (load_reason == LR_BACK) back_home();
            return;
        }
        if (r < 0) return;
        SS.offer = SS.sre; SS.offer_field = (uint32_t)g_vi_fields;
        for (int i = 0; i < MAXCL; i++) {
            if (SS.snap_for[i] && !SS.peer_started[i + 1]) offer_reset(i);   /* (one still loading: the newer state) */
            if (SS.snap_wait[i]) { SS.snap_wait[i] = 0; SS.snap_for[i] = 1; offer_reset(i); }
            if (SS.peer_started[i + 1]) SS.snap_for[i] = 0;
        }
        host_hash_pages();
        rt_log("net", "rejoin: the game at field %u (%.0f ms)", SS.offer_field, (rt_now_ns() - t) / 1e6);
        send_snap();
        return;
    }
    if (!SS.s0) SS.s0 = rt_snap_new();
    int r = rt_snap_take(SS.s0);
    if (r == 1) {                                     /* a load resumed here */
        if (load_reason == LR_JOIN) { load_reason = LR_NONE; game_begin(); }
        else if (load_reason == LR_BACK) back_home();
        return;
    }
    if (r < 0) { set_status("Couldn't start the online game (no save state)"); SS.phase = P_NONE; return; }
    size_t n; rt_snap_data(SS.s0, &n);
    SS.host = 1; SS.me = 0; SS.phase = P_NONE;
    SS.offer = SS.s0; SS.offer_field = (uint32_t)g_vi_fields;
    for (int i = 0; i < MAXCL; i++) SS.snap_for[i] = C[i].active;
    host_hash_pages();
    rt_log("net", "online game: the host's game at this field: %.1f MB, %u pages (%.0f ms)", n / 1048576.0, SS.npages, (rt_now_ns() - t) / 1e6);
    game_begin();
    send_snap();
    set_status("Sending the game to everyone...");
}
/* host: a client's WANT bitmap part -> once complete, its pages blob */
static void host_want(client_t *c, rbuf *r) {
    int ci = (int)(c - C);
    uint32_t id = r32(r), npages = r32(r), off = r32(r), nbytes = r16(r);
    if (r->bad || id != SS.id || npages != SS.npages || r->i + (int)nbytes > r->n || !SS.snap_for[ci] || !SS.offer) return;
    uint32_t bmbytes = (npages + 7) / 8, nparts = (bmbytes + 1023) / 1024;
    if (off % 1024 || off >= bmbytes || off + nbytes > bmbytes) return;
    if (SS.pages_blob[ci]) {                          /* (our PAGES got lost: again) */
        uint8_t b[16]; wbuf w = {b, 0, 16}; w32(&w, SS.id); w32(&w, SS.pages_size[ci]); w64(&w, SS.pages_hash[ci]); send_to(&c->addr, NM_PAGES, b, w.n); return;
    }
    if (!SS.want[ci]) { SS.want[ci] = calloc(1, bmbytes); SS.want_parts[ci] = calloc(1, nparts); SS.want_nparts[ci] = 0; }
    if (!SS.want_parts[ci][off / 1024]) { memcpy(SS.want[ci] + off, r->p + r->i, nbytes); SS.want_parts[ci][off / 1024] = 1; SS.want_nparts[ci]++; }
    if (SS.want_nparts[ci] < nparts) return;
    /* all in: the pages it asked for, each compressed */
    size_t sn; const uint8_t *s = rt_snap_data(SS.offer, &sn);
    const shdr *h = (const shdr *)s;
    uint32_t cap = 1u << 20, len = 0, nwant = 0; uint8_t *blob = malloc(cap);
    uint8_t tmp[PAGE + PAGE / 2];
    for (uint32_t p = 0; p < npages; p++) {
        if (!(SS.want[ci][p / 8] & (1 << (p % 8)))) continue;
        nwant++;
        uint32_t reg, plen; uint64_t addr, poff; page_info(h, p, &reg, &addr, &plen, &poff);
        const uint8_t *pg = s + poff;
        if (len + PAGE + 8 > cap) { cap *= 2; blob = realloc(blob, cap); }
        int zero = 1; for (uint32_t k = 0; k < plen; k++) if (pg[k]) { zero = 0; break; }
        if (zero) { blob[len++] = 0; blob[len++] = 0; continue; }
        int cl = lz_pack(pg, (int)plen, tmp, sizeof tmp);
        if (cl > 0 && cl < (int)plen) { blob[len++] = (uint8_t)cl; blob[len++] = (uint8_t)(cl >> 8); memcpy(blob + len, tmp, cl); len += cl; }
        else { blob[len++] = 0xFF; blob[len++] = 0xFF; memcpy(blob + len, pg, plen); len += plen; }
    }
    SS.pages_blob[ci] = blob; SS.pages_size[ci] = len; SS.pages_hash[ci] = fnv64(blob, len);
    rt_log("net", "%s needs %u of %u pages: %.1f MB to send", c->name, nwant, npages, len / 1048576.0);
    uint8_t b[16]; wbuf w = {b, 0, 16}; w32(&w, SS.id); w32(&w, len); w64(&w, SS.pages_hash[ci]); send_to(&c->addr, NM_PAGES, b, w.n);
}
/* client: SNAP arrived - freeze, get the host's game, load it (doesn't come back, except at
   the end of the online game, from our own save state) */
static int client_pump(fetch_t *f, int want_pages, uint32_t *pages_size, uint64_t *pages_hash) {
    lag_flush();
    for (int n = 0; n < 512; n++) {
        uint8_t buf[1500]; struct sockaddr_in from; socklen_t fl = sizeof from;
        int got = (int)recvfrom(sk, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (got < 8) { if (got <= 0) break; continue; }
        uint32_t m = buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24; int len = buf[6] | buf[7] << 8;
        if (m != NET_MAGIC || buf[5] != NET_VER || len > got - 8 || !same_addr(&from, &host_addr)) continue;
        rbuf r = {buf + 8, len, 0, 0};
        host_heard = ms();
        if (buf[4] == NM_CHUNK && f) fetch_chunk(f, &r);
        else if (buf[4] == NM_PAGES && want_pages) { if (r32(&r) == SS.id) { *pages_size = r32(&r); *pages_hash = r64(&r); } }
        else if (buf[4] == NM_BYE) return -1;
        else if (buf[4] == NM_PING) { uint64_t t = r64(&r); uint8_t b[8]; wbuf w = {b, 0, 8}; w64(&w, t); send_to(&host_addr, NM_PONG, b, w.n); }
    }
    plat_pump();
    if (ms() - host_heard > 30000) return -1;
    return 0;
}
static void client_load(rbuf *r, int resync) {
    uint32_t id = r32(r), base = r32(r), hsize = r32(r);
    shdr h; if (r->bad || id != SS.id || r->i + (int)sizeof h > r->n) return;
    memcpy(&h, r->p + r->i, sizeof h);
    uint32_t np = total_pages(&h);
    if (h.nreg > 54 || hsize != np * 8 || h.total > (1ull << 31)) { set_status("The host's game doesn't fit (a different version?)"); return; }
    SS.phase = P_LOADING;
    rt_log("net", "loading the host's game (field %u, %u pages)", base, np);
    /* our own game, to come back to (getting the game again: we already have it) */
    rt_det_enable();
    if (!resync) {
        if (!SS.own) SS.own = rt_snap_new();
        int rr = rt_snap_take(SS.own);
        if (rr == 1) { back_home(); return; }         /* (the online game is over: back in our game) */
        if (rr < 0) { set_status("Can't join: no save states on this PC"); SS.phase = P_READY; return; }
    }
    /* the page hashes */
    plat_title_note("Online: loading the host's game");
    fetch_t f; memset(&f, 0, sizeof f); fetch_start(&f, BLOB_HASHES, hsize);
    uint32_t psize = 0; uint64_t phash = 0;
    while (!fetch_done(&f)) { fetch_step(&f); if (client_pump(&f, 0, NULL, NULL) < 0) goto lost; plat_idle_wait(1000000); }
    uint64_t *hh = (uint64_t *)f.data; f.data = NULL;
    /* which pages differ from ours */
    uint32_t bmbytes = (np + 7) / 8; uint8_t *want = calloc(1, bmbytes); uint32_t nwant = 0;
    for (uint32_t p = 0; p < np; p++) {
        uint32_t reg, len; uint64_t addr, off; page_info(&h, p, &reg, &addr, &len, &off);
        int same = reg < 6 && XXH3_64bits((const void *)(uintptr_t)addr, len) == hh[p];   /* (stacks: always theirs) */
        if (!same) { want[p / 8] |= (uint8_t)(1 << (p % 8)); nwant++; }
    }
    rt_log("net", "need %u of %u pages from the host", nwant, np);
    uint64_t last = 0;
    while (!psize) {
        if (ms() - last > 300) {
            last = ms();
            for (uint32_t off = 0; off < bmbytes; off += 1024) {
                uint32_t n = bmbytes - off < 1024 ? bmbytes - off : 1024;
                uint8_t b[1100]; wbuf w = {b, 0, sizeof b}; w32(&w, SS.id); w32(&w, np); w32(&w, off); w16(&w, n);
                memcpy(b + w.n, want + off, n); w.n += (int)n; send_to(&host_addr, NM_WANT, b, w.n);
            }
        }
        if (client_pump(NULL, 1, &psize, &phash) < 0) goto lost;
        plat_idle_wait(1000000);
    }
    /* the pages */
    fetch_start(&f, BLOB_PAGES, psize);
    uint64_t t0 = ms(), shown = 0;
    while (!fetch_done(&f)) {
        fetch_step(&f); if (client_pump(&f, 0, NULL, NULL) < 0) goto lost;
        if (ms() - shown > 500) { shown = ms(); char t[96]; snprintf(t, sizeof t, "Online: loading the host's game (%u%%)", (unsigned)(100ull * f.ngot / (f.nch ? f.nch : 1))); plat_title_note(t); set_status("Loading the host's game... %u%%", (unsigned)(100ull * f.ngot / (f.nch ? f.nch : 1))); }
        plat_idle_wait(200000);
    }
    if (fnv64(f.data, f.size) != phash) { set_status("The host's game arrived damaged"); goto fail; }
    rt_log("net", "got %.1f MB in %.1f s", psize / 1048576.0, (ms() - t0) / 1000.0);
    /* put the host's state together: our pages + theirs, every page checked */
    uint8_t *st = malloc(h.total); if (!st) { set_status("Not enough memory to load the host's game"); goto fail; }
    memcpy(st, &h, sizeof h);
    uint32_t bp = 0;
    for (uint32_t p = 0; p < np; p++) {
        uint32_t reg, len; uint64_t addr, off; page_info(&h, p, &reg, &addr, &len, &off);
        uint8_t *dst = st + off;
        if (want[p / 8] & (1 << (p % 8))) {
            if (bp + 2 > f.size) goto bad;
            uint32_t code = f.data[bp] | f.data[bp + 1] << 8; bp += 2;
            if (code == 0) memset(dst, 0, len);
            else if (code == 0xFFFF) { if (bp + len > f.size) goto bad; memcpy(dst, f.data + bp, len); bp += len; }
            else { if (bp + code > f.size || lz_unpack(f.data + bp, (int)code, dst, (int)len) != (int)len) goto bad; bp += code; }
        } else memcpy(dst, (const void *)(uintptr_t)addr, len);
        if (XXH3_64bits(dst, len) != hh[p]) goto bad;
    }
    free(want); free(hh); free(f.data); free(f.got);
    rt_snap *hs = rt_snap_new();
    if (!rt_snap_set(hs, st, h.total)) { free(st); set_status("The host's game is from a different version of the game"); SS.phase = P_READY; return; }
    free(st);
    SS.host = 0; SS.me = my_id + 1;
    rt_log("net", "loading the host's game");
    load_reason = LR_JOIN;
    rb_stop(); g_resim = 0;
    rt_snap_load(hs);                                 /* -> net_snap_point (r == 1) on the host's field */
bad:
    set_status("The host's game arrived damaged (page check)");
fail:
    SS.phase = P_READY; plat_title_note(NULL);
    if (resync) { SS.phase = P_NONE; SS.resync = 0; SS.resync_sent = 0; request_resync("trying again"); }   /* (ask again) */
    return;
lost:
    set_status("Lost the host while loading its game"); SS.phase = P_NONE; plat_title_note(NULL);
    if (resync) game_end("lost the host while catching up");
}

/* ---- players dropping out and coming back ----
 * A PC that doesn't send its buttons in time (1.5 s) is "away": the host publishes the fields
 * without it (its players press nothing) and the game goes on for the others; when its buttons
 * for coming fields arrive again it is back. A PC that left (BYE: it closed the game) is gone;
 * it can join again (Multiplayer > Online > Join, same name): it gets a state of the game as it
 * is now and catches up (runs unpaced through the fields published meanwhile, asking the host
 * for them: SFREQ). The host itself going quiet: the others keep trying for 30 s. */
enum { NM_HELLO2_UNUSED = 43, NM_SFREQ, NM_SPEER, NM_RESYNC };
/* SPEER h->c: one PC's buttons (peer u8, newest field u32, count u8, players u8, count x players
   u32 - the host's own every field, a client's as soon as the host has them), then the field from
   which the host uses the receiver's own buttons (u32, 0xFFFFFFFF: not now - it's away). A client
   runs field F as soon as it has everyone's buttons for F (the "direct path"), or the host's
   published field (SFRAME), whichever comes first; every field it ran directly is checked against
   the published one. A difference (it was away and the host went on without it) or a state check
   failing: RESYNC c->h, and it loads the host's game again (like a rejoin, without leaving). */
#define NOT_NOW 0xFFFFFFFFu
static void rb_host_sinput(client_t *c, int pr, rbuf *r);
static void rb_client_speer(rbuf *r);
static void rb_client_sframe(rbuf *r);
#define AWAY_MS 1500
#define HOST_LOST_MS 30000
static const char *peer_name(int pr) { return pr >= 1 && pr <= MAXCL ? C[pr - 1].name : "the host"; }
static void hist_put(uint32_t fr, const uint32_t *b8) {
    uint32_t i = fr - SS.base;
    if (i >= SS.hist_cap) { uint32_t nc = SS.hist_cap ? SS.hist_cap * 2 : 4096; while (nc <= i) nc *= 2; SS.hist = realloc(SS.hist, nc * sizeof *SS.hist); SS.hist_cap = nc; }
    memcpy(SS.hist[i], b8, 32); if (i + 1 > SS.hist_n) SS.hist_n = i + 1;
}
static void send_fields(const struct sockaddr_in *to, uint32_t newest, int count) {
    uint8_t b[1400]; wbuf w = {b, 0, sizeof b}; w32(&w, newest); w8(&w, (uint32_t)count);
    for (int c = 0; c < count; c++) { uint32_t i = newest - (uint32_t)c - SS.base; for (int s = 0; s < 8; s++) w32(&w, i < SS.hist_n ? SS.hist[i][s] : 0); }
    send_to(to, NM_SFRAME, b, w.n);
}
/* host -> each other client: a PC's buttons the host will use (not absent, not published yet) */
static void send_speer(client_t *to, int pr, uint32_t newest, int count, int nl) {
    uint32_t lo = newest - (uint32_t)count + 1;
    if ((int32_t)(lo - SS.pub_next) < 0) lo = SS.pub_next;
    if ((int32_t)(newest - lo) < 0) return;
    if (newest - lo > 7) lo = newest - 7;
    int ci = (int)(to - C);
    uint8_t b[300]; wbuf w = {b, 0, sizeof b};
    w8(&w, (uint32_t)pr); w32(&w, newest); w8(&w, newest - lo + 1); w8(&w, (uint32_t)nl);
    for (uint32_t fr = newest + 1; fr-- > lo; ) { uint32_t k = fr % SRING;
        for (int l = 0; l < nl; l++) w32(&w, SS.pin_have[pr][k] && SS.pin_fr[pr][k] == fr ? SS.pin[pr][k][l] : 0); }
    w32(&w, SS.absent[ci + 1] || SS.gone[ci + 1] ? NOT_NOW : SS.back_from[ci + 1]);
    send_to(&to->addr, NM_SPEER, b, w.n);
}
static void forward_peer(int pr, uint32_t newest, int count, int nl) {
    for (int i = 0; i < MAXCL; i++) if (C[i].active && i + 1 != pr && SS.peer_started[i + 1] && !SS.gone[i + 1]) send_speer(&C[i], pr, newest, count, nl);
}
static void session_publish(void) {                 /* host: every field whose buttons are all in (or whose PC is away) */
    uint32_t first = SS.pub_next;
    for (;;) {
        uint32_t fr = SS.pub_next, k = fr % SRING;
        int missing = -1;
        for (int pr = 0; pr < SS.npeers; pr++) if (!SS.absent[pr] && (!SS.pin_have[pr][k] || SS.pin_fr[pr][k] != fr)) { missing = pr; break; }
        if (missing >= 0) {
            if (missing == 0) break;                  /* (our own buttons: next field) */
            if (!SS.stall_since) SS.stall_since = ms();
            if (ms() - SS.stall_since < AWAY_MS) break;
            SS.absent[missing] = 1; SS.stall_since = 0;   /* not in time: on without it */
            rt_log("net", "%s isn't answering - playing on without them", peer_name(missing));
            set_status("%s isn't answering - waiting for them to come back", peer_name(missing));
            continue;
        }
        SS.stall_since = 0;
        uint32_t b8[8];
        for (int s = 0; s < 8; s++) { int pr = SS.map_peer[s]; b8[s] = pr >= 0 && !SS.absent[pr] && SS.pin_have[pr][k] && SS.pin_fr[pr][k] == fr ? SS.pin[pr][k][SS.map_local[s]] : 0; }
        memcpy(SS.fr_btn[k], b8, 32); SS.fr_fr[k] = fr; SS.fr_have[k] = 1;
        hist_put(fr, b8);
        for (int pr = 0; pr < SS.npeers; pr++) SS.pin_have[pr][k] = 0;
        SS.pub_next++;
    }
    if (SS.pub_next == SS.base) return;
    static uint64_t last_sent;
    if (SS.pub_next != first || ms() - last_sent >= 30) {             /* (new fields, or again every 30 ms) */
        last_sent = ms();
        uint32_t newest = SS.pub_next - 1; int count = SS.pub_next - SS.base < 8 ? (int)(SS.pub_next - SS.base) : 8;
        for (int i = 0; i < MAXCL; i++) if (C[i].active && SS.peer_started[i + 1] && !SS.gone[i + 1]) send_fields(&C[i].addr, newest, count);
    }
}
static void offer_reset(int ci);
static void start_resync(int pr) {                  /* host: a state of the game for client PC pr, at the next field */
    int ci = pr - 1;
    SS.absent[pr] = 1; SS.peer_started[pr] = 0; SS.stall_since = 0; SS.peer_epoch[pr]++;
    offer_reset(ci); SS.snap_for[ci] = 0; SS.snap_wait[ci] = 1; SS.rejoin_offer[ci] = 0; SS.rejoin_pending = 1;
}
static void session_check(int pr, uint32_t fr) {    /* host: both hashes of field fr in? */
    int k = fr / 120 % 64;
    if (SS.desync || SS.hash_fr[k] != fr || SS.peer_hash_fr[pr][k] != fr) return;
    if (SS.hashes[k] == SS.peer_hash[pr][k]) return;
    rt_log("net", "OUT OF SYNC with %s at field %u - it gets our game again", peer_name(pr), fr);
    set_status("%s was out of sync - sending it the game again", peer_name(pr));
    SS.peer_hash_fr[pr][k] = 0;
    uint8_t b[4]; wbuf w = {b, 0, 4}; w32(&w, fr);
    if (pr >= 1 && C[pr - 1].active && !SS.gone[pr]) send_to(&C[pr - 1].addr, NM_SDESYNC, b, w.n);   /* (it asks: RESYNC) */
}
/* host: the session files as they are now (a rejoining player gets the game's NAND as it is) */
static void reload_session_files(void) {
    char d[1200]; session_dir(d, sizeof d);
    for (int i = 0; i < SS.nf; i++) {
        char p[1400]; snprintf(p, sizeof p, "%s/%s", d, SS.f[i].path);
        uint32_t sz; uint8_t *data = read_file(p, &sz);
        if (data) { free(SS.f[i].data); SS.f[i].data = data; SS.f[i].size = sz; SS.f[i].hash = fnv64(data, sz); }
    }
}
/* host: someone says HELLO while the game runs - a player who left, joining again */
static void rejoin_hello(const struct sockaddr_in *from, rbuf *r) {
    uint32_t nonce = hello_nonce(*r);
    char name[17]; rstr(r, name, 16); r8(r); uint32_t build = r32(r);
    if (r->bad || version_refused(from, build)) return;
    int ci = -1;
    for (int i = 0; i < MAXCL; i++) if (C[i].active && SS.gone[i + 1] && !strcmp(C[i].name, name)) ci = i;
    if (ci < 0) for (int i = 0; i < MAXCL; i++) if (C[i].active && SS.gone[i + 1]) { ci = i; break; }
    if (ci < 0) { const char *t = "An online game is running (only players who left can join it again)"; send_to(from, NM_REJECT, (const uint8_t *)t, (int)strlen(t)); return; }
    client_t *c = &C[ci];
    c->addr = *from; c->last_heard = ms();
    if (name[0]) snprintf(c->name, sizeof c->name, "%s", name);
    SS.gone[ci + 1] = 0; SS.absent[ci + 1] = 1; SS.peer_started[ci + 1] = 0; SS.peer_ready[ci] = 0;
    SS.snap_for[ci] = 0; SS.snap_wait[ci] = 0; offer_reset(ci); SS.rejoin_offer[ci] = 1; SS.offer_sent = 0; SS.peer_epoch[ci + 1] = 0;
    uint8_t b[16]; wbuf w = {b, 0, sizeof b};
    w8(&w, (uint32_t)ci); w8(&w, (uint32_t)c->players);
    for (int k = 0; k < MAXLOCAL; k++) w8(&w, k < c->players ? (uint32_t)c->slots[k] : 0xFF);
    w32(&w, (uint32_t)g_frames_presented); w32(&w, nonce);
    send_to(from, NM_WELCOME, b, w.n);
    reload_session_files();
    rt_log("net", "%s is joining the online game again", c->name);
    set_status("%s is joining again...", c->name);
}
/* client: get the host's game again (the field we ran differs, or the state check failed) */
static void request_resync(const char *why) {
    if (SS.resync || SS.host) return;
    uint64_t now = ms();
    if (now - SS.resync_t0 > 60000) { SS.resync_t0 = now; SS.nresync = 0; }
    if (++SS.nresync > 3) { SS.desync = 1; set_status("OUT OF SYNC - leave and join again"); return; }   /* (something is really wrong) */
    SS.resync = 1; SS.resync_sent = 0;
    set_status("Online: %s - loading the host's game again", why);
    plat_title_note("Online: catching up with the host");
}
static void resync_frame(void) {                    /* client: ask until the host's SNAP comes */
    if (!SS.resync || ms() - SS.resync_sent < 300) return;
    SS.resync_sent = ms();
    uint8_t b[4]; wbuf w = {b, 0, 4}; w32(&w, SS.id); send_to(&host_addr, NM_RESYNC, b, w.n);
}
static void session_packet(const struct sockaddr_in *from, int type, rbuf *r) {
    if (SS.host) {
        client_t *c = client_by_addr(from);
        if (!c) { if (type == NM_HELLO) rejoin_hello(from, r); return; }
        int pr = (int)(c - C) + 1;
        if (SS.gone[pr]) { if (type == NM_HELLO) rejoin_hello(from, r); return; }
        SS.peer_heard[pr] = ms(); c->last_heard = ms();
        if (type == NM_HELLO) {                       /* (a rejoiner whose WELCOME got lost) */
            uint8_t b[16]; wbuf w = {b, 0, sizeof b}; w8(&w, (uint32_t)(pr - 1)); w8(&w, (uint32_t)c->players);
            for (int k = 0; k < MAXLOCAL; k++) w8(&w, k < c->players ? (uint32_t)c->slots[k] : 0xFF);
            w32(&w, 0); w32(&w, hello_nonce(*r)); send_to(from, NM_WELCOME, b, w.n);
        }
        else if (type == NM_GETCHUNK) serve_chunk(c, r);
        else if (type == NM_WANT) host_want(c, r);
        else if (type == NM_READY) {                  /* a rejoiner has the files: a state of the game for it */
            if (r32(r) == SS.id && !SS.snap_for[pr - 1] && !SS.snap_wait[pr - 1]) { SS.snap_wait[pr - 1] = 1; SS.rejoin_offer[pr - 1] = 0; SS.rejoin_pending = 1; }
        }
        else if (type == NM_RESYNC) {                 /* it needs our game again (it fell out of step) */
            if (r32(r) != SS.id || r->bad) return;
            if (SS.peer_started[pr] && !SS.snap_wait[pr - 1]) {    /* (once: until it's in again) */
                rt_log("net", "%s gets the game again", c->name);
                start_resync(pr);
            }
        }
        else if (type == NM_SFREQ) {                  /* fields it missed (catching up) */
            uint32_t from_fr = r32(r);
            if (r->bad || from_fr < SS.base || from_fr >= SS.pub_next) return;
            for (int part = 0; part < 4 && from_fr < SS.pub_next; part++) {
                uint32_t n = SS.pub_next - from_fr; if (n > 40) n = 40;
                send_fields(from, from_fr + n - 1, (int)n); from_fr += n;
            }
        }
        else if (type == NM_SINPUT) {
            {   int cnt = r->n >= 6 ? r->p[4] : 0, nl0 = r->n >= 6 ? r->p[5] : 0;   /* (the epoch: after the buttons) */
                uint8_t ep = r->n == 6 + cnt * nl0 * 4 + 1 ? r->p[r->n - 1] : 0;
                if (ep != SS.peer_epoch[pr] || SS.snap_wait[pr - 1]) return; }       /* (from before it got the game again) */
            if (!SS.peer_started[pr]) { SS.peer_started[pr] = 1; SS.snap_for[pr - 1] = 0; rt_log("net", "%s is in the online game", c->name); }
            if (SS.rb) { rb_host_sinput(c, pr, r); return; }
            uint32_t newest = r32(r); int count = (int)r8(r), nl = (int)r8(r); if (nl > MAXLOCAL) nl = MAXLOCAL;
            for (int k2 = 0; k2 < count && !r->bad; k2++) {
                uint32_t fr = newest - (uint32_t)k2, k = fr % SRING; uint32_t bb[MAXLOCAL] = {0};
                for (int l = 0; l < nl; l++) bb[l] = r32(r);
                if (r->bad) break;
                if ((int32_t)(fr - SS.pub_next) < 0 || (int32_t)(fr - SS.pub_next) >= SRING) continue;
                if (!SS.pin_have[pr][k] || SS.pin_fr[pr][k] != fr) { memcpy(SS.pin[pr][k], bb, sizeof bb); SS.pin_fr[pr][k] = fr; SS.pin_have[pr][k] = 1; }
            }
            /* away and sending again for fields near the ones being published (not catching up):
               back - publishing waits for its buttons again (a few fields, until they're ahead) */
            if (SS.absent[pr]) { static uint64_t lg; if (ms() - lg > 1000) { lg = ms(); rt_log("net", "away %s sends field %u (bad %d), publishing %u", c->name, newest, r->bad, SS.pub_next); } }
            if (SS.absent[pr] && !r->bad && (int32_t)(newest - (SS.pub_next - 1)) >= -120) {
                SS.absent[pr] = 0; SS.stall_since = 0; SS.back_from[pr] = SS.pub_next;
                rt_log("net", "%s is back (from field %u)", c->name, SS.pub_next); set_status("%s is back", c->name); }
            if (!SS.absent[pr] && !r->bad) forward_peer(pr, newest, count, nl);
        } else if (type == NM_SHASH) {
            uint32_t fr = r32(r); uint64_t h = r64(r);
            if (r->bad || !fr || fr % 120) return;
            SS.peer_hash[pr][fr / 120 % 64] = h; SS.peer_hash_fr[pr][fr / 120 % 64] = fr;
            session_check(pr, fr);
        } else if (type == NM_PING) { uint64_t t = r64(r); uint8_t b[8]; wbuf w = {b, 0, 8}; w64(&w, t); send_to(from, NM_PONG, b, w.n); }
        else if (type == NM_PONG) { uint64_t t = r64(r); if (!r->bad) note_ping(c, (int)(ms() - t)); }
        else if (type == NM_BYE) {
            SS.gone[pr] = 1; SS.absent[pr] = 1; SS.peer_started[pr] = 0;
            rt_log("net", "%s left the online game (their players wait; they can join again)", c->name);
            set_status("%s left - they can join again (Multiplayer > Online > Join)", c->name);
        }
        return;
    }
    if (!same_addr(from, &host_addr)) return;
    SS.host_heard = ms();
    if (type == NM_SFRAME && SS.rb) rb_client_sframe(r);
    else if (type == NM_SPEER && SS.rb) rb_client_speer(r);
    else if (type == NM_PONG) { uint64_t t = r64(r); if (!r->bad) my_ping = (int)(ms() - t); }
    else if (type == NM_SFRAME) {
        extern uint64_t g_vi_fields;
        uint32_t newest = r32(r), cur = (uint32_t)g_vi_fields; int count = (int)r8(r);
        if (!r->bad && (int32_t)(newest - SS.newest_seen) > 0) SS.newest_seen = newest;
        for (int c = 0; c < count && !r->bad; c++) {
            uint32_t fr = newest - (uint32_t)c, k = fr % SRING; uint32_t bb[8];
            for (int s = 0; s < 8; s++) bb[s] = r32(r);
            if (r->bad) break;
            if ((int32_t)(fr - cur) >= SRING - 16) continue;   /* (far ahead while catching up: would overwrite fields still to come) */
            if (SS.ran_have[k] && SS.ran_fr[k] == fr) {          /* ran it directly: was that what the host played? */
                SS.ran_have[k] = 0;
                if (memcmp(SS.ran_btn[k], bb, sizeof bb) && !SS.resync) {
                    rt_log("net", "field %u differs from the host's (we were away?) - getting its game again", fr);
                    request_resync("this PC fell out of step");
                }
                continue;
            }
            if (!SS.fr_have[k] || SS.fr_fr[k] != fr) { memcpy(SS.fr_btn[k], bb, sizeof bb); SS.fr_fr[k] = fr; SS.fr_have[k] = 1; }
        }
    } else if (type == NM_SPEER) {
        int pr = (int)r8(r); uint32_t newest = r32(r); int count = (int)r8(r), nl = (int)r8(r);
        if (r->bad || pr < 0 || pr >= MAXPEER || pr == SS.me || count > 8 || nl > MAXLOCAL) return;
        for (int c = 0; c < count && !r->bad; c++) {
            uint32_t fr = newest - (uint32_t)c, k = fr % SRING; uint32_t bb[MAXLOCAL] = {0};
            for (int l = 0; l < nl; l++) bb[l] = r32(r);
            if (r->bad) return;
            if (!SS.pin_have[pr][k] || SS.pin_fr[pr][k] != fr) { memcpy(SS.pin[pr][k], bb, sizeof bb); SS.pin_fr[pr][k] = fr; SS.pin_have[pr][k] = 1; }
        }
        uint32_t from = r32(r);
        if (!r->bad && pr == 0 && (!SS.my_from_known || (int32_t)(newest - SS.my_from_stamp) >= 0)) {   /* (the newest word on it) */
            SS.my_from_stamp = newest;
            if (from == NOT_NOW) SS.my_from_known = 0; else { SS.my_from = from; SS.my_from_known = 1; }
        }
    } else if (type == NM_SNAP) {
        if (SS.resync && SS.phase != P_LOADING) client_load(r, 1);
    } else if (type == NM_SDESYNC) {
        SS.desync_fr = r32(r); rt_log("net", "OUT OF SYNC at field %u - getting the host's game again", SS.desync_fr);
        request_resync("out of sync");
    }
    else if (type == NM_PING) { uint64_t t = r64(r); uint8_t b[8]; wbuf w = {b, 0, 8}; w64(&w, t); send_to(&host_addr, NM_PONG, b, w.n); }
    else if (type == NM_BYE) game_end("the host closed the online game");
}
static void session_poll(void) {
    if (sk == BAD_SOCK) return;
    lag_flush();
    for (int n = 0; n < 512; n++) {
        uint8_t buf[1500]; struct sockaddr_in from; socklen_t fl = sizeof from;
        int got = (int)recvfrom(sk, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (got < 8) { if (got <= 0) break; continue; }
        if (blip_until && ms() < blip_until) continue;
        uint32_t m = buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24; int len = buf[6] | buf[7] << 8;
        if (m != NET_MAGIC || buf[5] != NET_VER || len > got - 8) continue;
        rbuf r = {buf + 8, len, 0, 0};
        session_packet(&from, buf[4], &r);
        if (!SS.on) return;
    }
    if (SS.host) {
        if (!SS.rb) session_publish();
        int waiting = 0; for (int i = 0; i < MAXCL; i++) if (C[i].active && SS.snap_for[i] && !SS.pages_blob[i] && !SS.gone[i + 1]) waiting = 1;
        if (waiting && ms() - SS.snap_sent > 300) send_snap();
        if (ms() - SS.offer_sent > 250) { SS.offer_sent = ms(); for (int i = 0; i < MAXCL; i++) if (C[i].active && SS.rejoin_offer[i] && !SS.gone[i + 1]) send_offer(&C[i]); }
        if (g_frames_presented % 60 == 0) { static uint64_t pinged; if (ms() - pinged > 900) { pinged = ms();   /* (keep-alive both ways) */
            uint8_t pb[8]; wbuf pw = {pb, 0, 8}; w64(&pw, ms()); for (int i = 0; i < MAXCL; i++) if (C[i].active && !SS.gone[i + 1]) send_to(&C[i].addr, NM_PING, pb, pw.n); } }
    }
}

/* ======================================================================== rollback
 * Every PC runs field F as soon as it gets there: with its own players' buttons for F (input
 * delay 0 by default: what you press is in this very field) and, for the other PCs, their
 * buttons for F if they're here, else a guess (the last ones known). Each field's state is kept
 * (rt_state.c rb_save, ~0.6 MB of changed pages). When buttons arrive that differ from the
 * guess a field ran with, the game goes back to that field's state and runs the fields since
 * again, silently (g_resim: not drawn, not heard), then goes on.
 * The host still decides: it forwards each PC's buttons (SPEER) to the others, publishes every
 * field whose buttons are all in (SFRAME, history for catching up), and decides for a PC that
 * went quiet (1.5 s): no buttons from where it stopped, until its own come in again. That PC
 * gets those decisions too (SPEER with its own number) and goes back if it can, or gets the
 * game again (RESYNC). Nobody runs more than RB_WINDOW fields ahead of a PC's last known
 * buttons; a PC that is ahead of the others slows down a little (time sync). */
#define RB_WINDOW 20                                  /* (fields: ~330 ms of not knowing) */
void det_pace_reset(void);
void det_pace_slow(uint64_t ns);
static uint32_t cur_field(void) { extern uint64_t g_vi_fields; return (uint32_t)g_vi_fields; }
static void rb_conf_advance(int pr) {
    for (;;) { uint32_t f = SS.conf[pr] + 1, k = f % SRING; if (SS.pin_have[pr][k] && SS.pin_fr[pr][k] == f) SS.conf[pr] = f; else break; }
}
/* buttons of PC pr for field f (known: 1), else the guess */
static uint32_t rb_input(int pr, uint32_t f, int l, int *known) {
    uint32_t k = f % SRING;
    if (SS.pin_have[pr][k] && SS.pin_fr[pr][k] == f) { if (known) *known = 1; return SS.pin[pr][k][l]; }
    if (known) *known = 0;
    uint32_t c = SS.conf[pr], kc = c % SRING;
    return SS.pin_have[pr][kc] && SS.pin_fr[pr][kc] == c ? SS.pin[pr][kc][l] : 0;
}
/* PC pr's buttons for field f. override: the host's word (replaces ours). A field that already
   ran with something else is noted (SS.mis): the game goes back to it */
static void rb_store(int pr, uint32_t f, const uint32_t *bb, int override) {
    uint32_t k = f % SRING, now = cur_field();
    if ((int32_t)(f - SS.base) < 0 || (int32_t)(f - now) >= SRING - 64 || (int32_t)(now - f) >= SRING - 64) return;
    int had = SS.pin_have[pr][k] && SS.pin_fr[pr][k] == f;
    if (had && (!override || !memcmp(SS.pin[pr][k], bb, sizeof SS.pin[pr][k]))) return;
    memcpy(SS.pin[pr][k], bb, sizeof SS.pin[pr][k]); SS.pin_fr[pr][k] = f; SS.pin_have[pr][k] = 1;
    rb_conf_advance(pr);
    /* (already ran, or is running - it was decided this field: with these?) */
    if (SS.used_fr[k] == f && ((int32_t)(f - now) < 0 || (f == now && SS.decided == now)))
        for (int s = 0; s < 8; s++) if (SS.map_peer[s] == pr && SS.used[k][s] != bb[SS.map_local[s]]) {
            if (SS.mis == NOMIS || (int32_t)(f - SS.mis) < 0) SS.mis = f;
            break;
        }
}
/* host -> client: PC pr's buttons for fields lo..hi (newest first), the host's field, and how far
   the host has the receiver's own */
static void rb_send_peer(client_t *to, int pr, uint32_t lo, uint32_t hi) {
    if ((int32_t)(hi - lo) < 0) return;
    if (hi - lo > 31) lo = hi - 31;
    int ci = (int)(to - C);
    uint8_t b[700]; wbuf w = {b, 0, sizeof b};
    w8(&w, (uint32_t)pr); w32(&w, hi); w8(&w, hi - lo + 1); w8(&w, MAXLOCAL);
    for (uint32_t f = hi + 1; f-- > lo; ) { uint32_t k = f % SRING;
        for (int l = 0; l < MAXLOCAL; l++) w32(&w, SS.pin_have[pr][k] && SS.pin_fr[pr][k] == f ? SS.pin[pr][k][l] : 0); }
    w32(&w, cur_field()); w32(&w, SS.conf[ci + 1]);
    send_to(&to->addr, NM_SPEER, b, w.n);
}
static void rb_forward(int pr, uint32_t lo, uint32_t hi, int to_self) {
    for (int i = 0; i < MAXCL; i++) if (C[i].active && SS.peer_started[i + 1] && !SS.gone[i + 1] && (i + 1 != pr || to_self)) rb_send_peer(&C[i], pr, lo, hi);
}
/* host: a client's buttons */
static void rb_host_sinput(client_t *c, int pr, rbuf *r) {
    uint32_t newest = r32(r); int count = (int)r8(r), nl = (int)r8(r);
    if (r->bad || nl > MAXLOCAL || count > 32) return;
    uint32_t lo = NOMIS, hi = 0;
    /* away, and these are about now (not old ones arriving late): back - from the field after the
       last one decided for it, where its own buttons count again */
    if (SS.absent[pr] && !SS.gone[pr] && (int32_t)(newest + 30 - SS.synth_to[pr]) >= 0) {
        SS.absent[pr] = 0; rt_log("net", "%s is back (from field %u)", c->name, SS.synth_to[pr] + 1); set_status("%s is back", c->name);
    }
    for (int k2 = 0; k2 < count; k2++) {
        uint32_t f = newest - (uint32_t)k2, bb[MAXLOCAL] = {0};
        for (int l = 0; l < nl; l++) bb[l] = r32(r);
        if (r->bad) return;
        if ((int32_t)(f - SS.synth_to[pr]) <= 0) continue;           /* (decided already: it was away) */
        uint32_t k = f % SRING; int had = SS.pin_have[pr][k] && SS.pin_fr[pr][k] == f;
        if (had) continue;
        rb_store(pr, f, bb, 0);
        if (lo == NOMIS || (int32_t)(f - lo) < 0) lo = f;
        if ((int32_t)(f - hi) > 0 || hi == 0) hi = f;
    }
    SS.peer_frame[pr] = newest - (uint32_t)SS.delay; SS.peer_frame_t[pr] = ms();
    if (lo != NOMIS) rb_forward(pr, lo, hi, 0);
}
/* client: a PC's buttons from the host */
static void rb_client_speer(rbuf *r) {
    int pr = (int)r8(r); uint32_t newest = r32(r); int count = (int)r8(r), nl = (int)r8(r);
    if (r->bad || pr < 0 || pr >= MAXPEER || count > 32 || nl > MAXLOCAL) return;
    uint32_t vals[32][MAXLOCAL];
    for (int c = 0; c < count; c++) { memset(vals[c], 0, sizeof vals[c]); for (int l = 0; l < nl; l++) vals[c][l] = r32(r); }
    uint32_t hf = r32(r), ack = r32(r);
    if (r->bad) return;
    for (int c = count; c-- > 0; ) rb_store(pr, newest - (uint32_t)c, vals[c], 1);   /* (oldest first) */
    SS.peer_frame[0] = hf; SS.peer_frame_t[0] = ms();
    if ((int32_t)(ack - SS.my_ack) > 0) SS.my_ack = ack;
}
/* client: a published field (all of everyone's buttons, the host's word) */
static void rb_client_sframe(rbuf *r) {
    uint32_t newest = r32(r); int count = (int)r8(r);
    if (r->bad) return;
    if ((int32_t)(newest - SS.newest_seen) > 0) SS.newest_seen = newest;
    if ((int32_t)(newest - (uint32_t)count - SS.final_to) <= 0 && (int32_t)(newest - SS.final_to) > 0) SS.final_to = newest;   /* (no gap) */
    for (int c = 0; c < count && !r->bad; c++) {
        uint32_t f = newest - (uint32_t)c, b8[8];
        for (int s = 0; s < 8; s++) b8[s] = r32(r);
        if (r->bad) break;
        for (int pr = 0; pr < SS.npeers; pr++) {
            uint32_t bb[MAXLOCAL] = {0}; int any = 0;
            for (int s = 0; s < 8; s++) if (SS.map_peer[s] == pr) { bb[SS.map_local[s]] = b8[s]; any = 1; }
            if (any) rb_store(pr, f, bb, 1);
        }
    }
}
/* host: fields whose buttons are all in -> published (history + SFRAME) */
static void rb_publish(void) {
    uint32_t first = SS.pub_next;
    for (;;) {
        uint32_t f = SS.pub_next, k = f % SRING; int all = 1;
        for (int pr = 0; pr < SS.npeers && all; pr++) if (!(SS.pin_have[pr][k] && SS.pin_fr[pr][k] == f)) all = 0;
        if (!all || (int32_t)(f - cur_field()) > (int32_t)SS.delay + 1) break;
        uint32_t b8[8];
        for (int s = 0; s < 8; s++) { int pr = SS.map_peer[s]; b8[s] = pr >= 0 ? SS.pin[pr][k][SS.map_local[s]] : 0; }
        hist_put(f, b8);
        SS.pub_next++;
    }
    static uint64_t last_sent;
    if (SS.pub_next == SS.base) return;
    if (SS.pub_next != first || ms() - last_sent >= 30) {
        last_sent = ms();
        uint32_t newest = SS.pub_next - 1; int count = SS.pub_next - SS.base < 8 ? (int)(SS.pub_next - SS.base) : 8;
        for (int i = 0; i < MAXCL; i++) if (C[i].active && SS.peer_started[i + 1] && !SS.gone[i + 1]) send_fields(&C[i].addr, newest, count);
    }
}
/* host: a PC that's away (or left): no buttons for it up to `upto`, everyone told */
static void rb_synth(int pr, uint32_t upto) {
    uint32_t from = SS.conf[pr] + 1;
    if ((int32_t)(upto - from) < 0) return;
    uint32_t zero[MAXLOCAL] = {0};
    for (uint32_t f = from; (int32_t)(f - upto) <= 0; f++) rb_store(pr, f, zero, 0);
    SS.synth_to[pr] = upto;
    for (uint32_t lo = from; (int32_t)(lo - upto) <= 0; lo += 32) { uint32_t hi = lo + 31; if ((int32_t)(hi - upto) > 0) hi = upto; rb_forward(pr, lo, hi, 1); }
}
static void rb_host_duties(uint32_t F) {
    uint32_t horizon = F + (uint32_t)SS.delay;
    for (int pr = 1; pr < SS.npeers; pr++) if (SS.absent[pr] || SS.gone[pr]) rb_synth(pr, horizon);
    rb_publish();
}
static int rb_min_conf(uint32_t *minc, int *who) {     /* the PC whose buttons we have least of */
    int w = -1; uint32_t m = 0;
    for (int pr = 0; pr < SS.npeers; pr++) if (pr != SS.me) { if (w < 0 || (int32_t)(SS.conf[pr] - m) < 0) { m = SS.conf[pr]; w = pr; } }
    if (w < 0) { m = cur_field() + 1000; }
    *minc = m; if (who) *who = w; return w >= 0;
}
static void rb_send_mine(uint32_t T) {               /* our buttons up to field T */
    if (SS.host) { rb_forward(0, T >= 15 ? T - 15 : 0, T, 0); return; }
    /* everything the host doesn't have yet (it says how far it has them), in parts of 32 - up to
       4 parts, the newest first; the newest part is also what is sent again while waiting */
    uint32_t lo = SS.my_ack + 1;
    if ((int32_t)(T - lo) > 127 || (int32_t)(lo - SS.base) < 0) lo = (int32_t)(T - 127 - SS.base) > 0 ? T - 127 : SS.base;
    if ((int32_t)(T - lo) < 0) lo = T;                        /* (all acked: the newest again anyway) */
    for (uint32_t hi = T; ; hi -= 32) {
        uint32_t plo = (int32_t)(hi - lo) > 31 ? hi - 31 : lo;
        int count = (int)(hi - plo + 1);
        uint8_t pb[700]; uint8_t *buf = hi == T ? SS.last_sinput : pb;
        wbuf w = {buf, 0, sizeof pb};
        w32(&w, hi); w8(&w, (uint32_t)count); w8(&w, MAXLOCAL);
        for (int c = 0; c < count; c++) { uint32_t f = hi - (uint32_t)c;
            for (int l = 0; l < MAXLOCAL; l++) w32(&w, rb_input(SS.me, f, l, NULL)); }
        w8(&w, SS.epoch);
        if (hi == T) SS.last_sinput_n = w.n;
        if (!SS.resync) send_to(&host_addr, NM_SINPUT, buf, w.n);
        if (plo == lo) break;
    }
    SS.last_input_sent = ms();
}
/* every video field (rt_det.c), in a rollback game */
static void rb_field(void) {
    uint32_t F = cur_field();
    int final = 1;                                    /* (everyone's buttons for F are the host's last word) */
    if (SS.host) { for (int pr = 0; pr < SS.npeers; pr++) if (!SS.gone[pr] && (int32_t)(SS.conf[pr] - F) < 0) final = 0; }
    else final = (int32_t)(SS.final_to - F) >= 0;
    int resumed = rb_save(F, !final) == 1;            /* (1: we came back here to run it again) */
    if (resumed) SS.in_resim = 1;
    g_resim = (int32_t)(SS.resim_to - F) > 0;
    if (!g_resim && SS.in_resim) {                   /* (caught up again: from now on in time) */
        SS.in_resim = 0; det_pace_reset();
        SS.st_rb_ns += rt_now_ns() - SS.stall_t0 * 0;
    }
    /* our buttons for F + delay (once - not again while running fields again) */
    uint32_t T = F + (uint32_t)SS.delay;
    if (!g_resim) {
        uint32_t kt = T % SRING;
        if (!(SS.pin_have[SS.me][kt] && SS.pin_fr[SS.me][kt] == T)) {
            uint32_t bb[MAXLOCAL] = {0};
            for (int l = 0; l < SS.nlocal && l < MAXLOCAL; l++) bb[l] = remotes_local_buttons(l);
            rb_store(SS.me, T, bb, 0);
        }
        if (!g_det_fast) rb_send_mine(T);
        if (!SS.host) {
            resync_frame();
            static uint64_t pinged; if (ms() - pinged > 1000) { pinged = ms(); uint8_t pb[8]; wbuf pw = {pb, 0, 8}; w64(&pw, ms()); send_to(&host_addr, NM_PING, pb, pw.n); }
            /* catching up: the published fields we don't have yet */
            uint32_t mc; rb_min_conf(&mc, NULL);
            if (g_det_fast && (int32_t)(mc - (F + 30)) < 0 && ms() - SS.last_req > 60) {
                SS.last_req = ms(); uint8_t b[4]; wbuf w = {b, 0, 4}; w32(&w, mc + 1); send_to(&host_addr, NM_SFREQ, b, w.n); }
        }
    }
    session_poll();
    if (!SS.on) return;
    if (SS.host && !g_resim) rb_host_duties(F);
    /* a wrong guess: back to that field */
    if (SS.mis != NOMIS && (int32_t)(SS.mis - F) < 0) {
        uint32_t R = SS.mis; SS.mis = NOMIS;
        if (rb_can_go_back(R)) {
            if ((int32_t)(F - SS.resim_to) > 0 || !g_resim) SS.resim_to = F;
            SS.st_rollbacks++; SS.st_rb_fields += F - R;
            rb_rollback(R);                           /* (doesn't return: rb_save of field R returns 1) */
        }
        if (!SS.host) request_resync("fell too far out of step");
        else rt_log("net", "rollback to field %u not possible (newest kept %u)", R, rb_newest());
    }
    if (!g_resim) {
        /* not too far ahead of anyone's known buttons; the host also waits for everyone to start */
        uint64_t t0 = ms(); int shown = 0;
        for (;;) {
            uint32_t mc; int who; rb_min_conf(&mc, &who);
            int start_wait = 0;
            if (SS.host) for (int i = 0; i < MAXCL; i++) if (C[i].active && !SS.peer_started[i + 1] && !SS.gone[i + 1] && !SS.absent[i + 1]) start_wait = 1;
            if (!start_wait && (who < 0 || (int32_t)(F - mc) <= RB_WINDOW)) break;
            uint64_t now = ms();
            if (SS.host && now - t0 > (start_wait ? 60000u : AWAY_MS)) {   /* on without them */
                for (int pr = 1; pr < SS.npeers; pr++) {
                    int late = start_wait ? !SS.peer_started[pr] && !SS.gone[pr] : (int32_t)(F - SS.conf[pr]) > RB_WINDOW;
                    if (late && !SS.absent[pr] && !SS.gone[pr]) {
                        SS.absent[pr] = 1; rt_log("net", "%s isn't answering - playing on without them", peer_name(pr));
                        set_status("%s isn't answering - waiting for them to come back", peer_name(pr));
                    }
                }
                rb_host_duties(F); t0 = now;
                continue;
            }
            if (!SS.host && now - SS.host_heard > HOST_LOST_MS) game_end("lost the connection to the host");
            if (!SS.host && now - SS.host_heard > 2000) { char n[96]; snprintf(n, sizeof n, "Online: reconnecting to the host (%llu s)", (unsigned long long)((now - SS.host_heard) / 1000)); plat_title_note(n); shown = 1; }
            else if (now - t0 > 1000) { plat_title_note(start_wait ? "Online: waiting for everyone to load the game" : "Online: waiting for the other players"); shown = 1; }
            if (!SS.host && now - SS.last_input_sent > 100) rb_send_mine(T);   /* (again: some may be lost) */
            /* client: a gap in what we have (lost packets) - the host's published fields from there */
            if (!SS.host && now - t0 > 100 && now - SS.last_req > 150) {
                SS.last_req = now; uint8_t b[4]; wbuf w = {b, 0, 4}; w32(&w, mc + 1); send_to(&host_addr, NM_SFREQ, b, w.n); }
            plat_pump(); plat_idle_wait(1000000);
            session_poll(); if (!SS.on) return;
            if (SS.host) rb_host_duties(F);
            if (SS.mis != NOMIS && (int32_t)(SS.mis - F) < 0) break;           /* (go back first) */
        }
        SS.st_stall_ms += ms() - t0;
        if (SS.mis != NOMIS && (int32_t)(SS.mis - F) < 0) {
            uint32_t R = SS.mis; SS.mis = NOMIS;
            if (rb_can_go_back(R)) { SS.resim_to = F; SS.st_rollbacks++; SS.st_rb_fields += F - R; rb_rollback(R); }
            if (!SS.host) request_resync("fell too far out of step");
        }
        (void)shown;
        /* time sync: ahead of the others (by what we know of them + half a round trip)? a moment */
        {   int ahead = 0;
            for (int pr = 0; pr < SS.npeers; pr++) if (pr != SS.me && SS.peer_frame_t[pr] && !SS.absent[pr] && !SS.gone[pr]) {
                int rtt = SS.host ? (pr >= 1 ? C[pr - 1].ping : 0) : my_ping;
                int64_t est = (int64_t)SS.peer_frame[pr] + (int64_t)((ms() - SS.peer_frame_t[pr]) + (uint64_t)(rtt / 2)) * 60 / 1000;
                int a = (int)((int64_t)F - est); if (a > ahead) ahead = a;
            }
            /* (the pacing's own clock moves, so it doesn't make up for it by sleeping less later) */
            if (ahead >= 2) { uint64_t ns = ahead >= 4 ? 4000000ull : 2000000ull; det_pace_slow(ns); SS.st_sync_ms += ns / 1000000; }
        }
        /* far behind (just joined again, or back after a drop): run silently and fast until close */
        int was_fast = g_det_fast;
        g_det_fast = !SS.host && SS.peer_frame_t[0] && (int32_t)(SS.peer_frame[0] - F) > 8;
        if (g_det_fast) g_resim = 1;
        if (g_det_fast && !was_fast) { SS.fast_t0 = ms(); SS.fast_f0 = F; }
        if (!g_det_fast && was_fast) {
            uint64_t t = ms() - SS.fast_t0;
            rt_log("net", "caught up: %u fields in %.1f s (%.0f fields/s; the game runs 60)", F - SS.fast_f0, t / 1000.0, t ? (F - SS.fast_f0) * 1000.0 / t : 0.0);
            det_pace_reset();
        }
    }
    /* the buttons field F runs with */
    uint32_t k = F % SRING;
    for (int s = 0; s < 8; s++) { int pr = SS.map_peer[s]; SS.cur_btn[s] = pr >= 0 ? rb_input(pr, F, SS.map_local[s], NULL) : 0; }
    memcpy(SS.used[k], SS.cur_btn, sizeof SS.cur_btn); SS.used_fr[k] = F; SS.decided = F;
    {   static FILE *ul; static int ui = -1; if (ui < 0) { const char *e = getenv("RT_NET_LOG_USED"); ui = e != NULL; if (e) ul = fopen(e, "w"); }   /* (testing: the buttons each field ran with) */
        if (ul) { fprintf(ul, "%u", F); for (int s = 0; s < 8; s++) fprintf(ul, " %x", SS.cur_btn[s]); fprintf(ul, "%s\n", g_resim ? " r" : ""); } }
    /* state checks: computed at every 120th field, sent once every field before it is final */
    if (F % 120 == 0 && F) { int i = F / 120 % 64; SS.rh[i] = det_state_hash(); SS.rh_fr[i] = F; SS.rh_done[i] = 0; }
    {   uint32_t mc; rb_min_conf(&mc, NULL);
        for (int i = 0; i < 64; i++) if (SS.rh_fr[i] && !SS.rh_done[i] && (int32_t)(mc - (SS.rh_fr[i] - 1)) >= 0 && (int32_t)(F - SS.rh_fr[i]) >= 0 && SS.mis == NOMIS) {
            uint32_t hf = SS.rh_fr[i]; SS.rh_done[i] = 1;
            {   static int lg = -1; if (lg < 0) lg = getenv("RT_NET_LOG_HASH") != NULL; if (lg) rt_log("net", "session: field %u hash %016llX", hf, (unsigned long long)SS.rh[i]); }
            if (SS.host) { SS.hashes[hf / 120 % 64] = SS.rh[i]; SS.hash_fr[hf / 120 % 64] = hf; for (int pr = 1; pr < SS.npeers; pr++) session_check(pr, hf); }
            else { uint8_t b[12]; wbuf w = {b, 0, 12}; w32(&w, hf); w64(&w, SS.rh[i]); send_to(&host_addr, NM_SHASH, b, w.n); }
        }
    }
    {   static long bad = -2; if (bad == -2) { const char *e = getenv("RT_NET_TEST_DESYNC"); bad = e ? atol(e) : -1; }   /* testing: make this PC differ */
        if (bad >= 0 && F == (uint32_t)bad && !g_resim) { W8(0x80001F00, R8(0x80001F00) ^ 0x5A); bad = -1; } }
    if (!g_resim) {                                    /* the window title, the log */
        static uint32_t since; static uint64_t last_note;
        if (ms() - last_note > 500) { last_note = ms();
            int away = 0; for (int pr = 1; pr < SS.npeers; pr++) if (SS.absent[pr] || SS.gone[pr]) away = 1;
            char n[128]; snprintf(n, sizeof n, "%s%s%s", SS.desync ? "Online: OUT OF SYNC" : SS.host ? "Online (host)" : "Online", away ? " - a player is away" : "", g_det_fast ? " - catching up" : "");
            plat_title_note(n); }
        if (F - since >= 600) {
            double save_ms; uint64_t saves, skips; rb_stats(&save_ms, &saves, &skips);
            rt_log("net", "session: field %u: %llu rollbacks (%.1f fields back on average), waited %llu ms, slowed %llu ms, state %.2f ms/field (%llu kept, %llu final)",
                   F, (unsigned long long)SS.st_rollbacks, SS.st_rollbacks ? (double)SS.st_rb_fields / SS.st_rollbacks : 0.0,
                   (unsigned long long)SS.st_stall_ms, (unsigned long long)SS.st_sync_ms, save_ms, (unsigned long long)saves, (unsigned long long)skips);
            SS.st_rollbacks = SS.st_rb_fields = SS.st_stall_ms = SS.st_sync_ms = 0; since = F;
        }
    }
}
/* client: field F from everyone's buttons, if they're all here (and the host counts ours for F) */
static int direct_field(uint32_t F, uint32_t *b8) {
    static int off = -1; if (off < 0) off = getenv("RT_NET_NO_DIRECT") != NULL;   /* (testing: the host's fields only, as before) */
    if (off || SS.rb_session || SS.resync || !SS.my_from_known || (int32_t)(F - SS.my_from) < 0 || (int32_t)(F - SS.base) < 0) return 0;
    uint32_t k = F % SRING, out[8];
    for (int s = 0; s < 8; s++) {
        int pr = SS.map_peer[s];
        if (pr < 0) out[s] = 0;
        else if (pr == SS.me) out[s] = SS.my_in[k][SS.map_local[s]];
        else if (SS.pin_have[pr][k] && SS.pin_fr[pr][k] == F) out[s] = SS.pin[pr][k][SS.map_local[s]];
        else return 0;
    }
    memcpy(b8, out, sizeof out);
    return 1;
}
/* every video field (rt_det.c): send my buttons for field F + delay, then wait for field F */
static int test_hooks(uint32_t F) {                 /* testing (both kinds of game): 1 = the session ended */
    {   static long at = -2; if (at == -2) { const char *e = getenv("RT_NET_LEAVE_AT"); at = e ? atol(e) : -1; }   /* testing: leave at that field */
        if (at >= 0 && F >= (uint32_t)at) { at = -1; net_leave_session(); return 1; } }
    {   static long at = -2; if (at == -2) { const char *e = getenv("RT_NET_QUIT_AT"); at = e ? atol(e) : -1; }    /* testing: close the game at that field */
        if (at >= 0 && F >= (uint32_t)at) { rt_log("net", "test: closing the game"); net_shutdown(); exit(0); } }
    {   static long at = -2, len; if (at == -2) { const char *e = getenv("RT_NET_BLIP_AT"); at = -1; if (e) sscanf(e, "%ld:%ld", &at, &len); }   /* testing: the network drops for len ms */
        if (at >= 0 && F >= (uint32_t)at) { at = -1; blip_until = ms() + (uint64_t)len; rt_log("net", "test: network down for %ld ms", len); } }
    {   static long at = -2, len; if (at == -2) { const char *e = getenv("RT_NET_BLIP_OUT_AT"); at = -1; if (e) sscanf(e, "%ld:%ld", &at, &len); }   /* testing: only our packets are lost */
        if (at >= 0 && F >= (uint32_t)at) { at = -1; blip_out_until = ms() + (uint64_t)len; rt_log("net", "test: our packets lost for %ld ms", len); } }
    return 0;
}
void session_field(void) {
    {   extern uint64_t g_vi_fields; if (!g_resim && test_hooks((uint32_t)g_vi_fields)) return; }
    if (SS.rb && rb_active()) { rb_field(); return; }
    extern uint64_t g_vi_fields;
    uint32_t F = (uint32_t)g_vi_fields;
    uint32_t T = F + (uint32_t)SS.delay, kt = T % SRING;
    if (!SS.host) resync_frame();
    for (int l = 0; l < MAXLOCAL; l++) SS.my_in[kt][l] = l < SS.nlocal ? remotes_local_buttons(l) : 0;
    if (SS.host) {
        memcpy(SS.pin[0][kt], SS.my_in[kt], sizeof SS.my_in[kt]); SS.pin_fr[0][kt] = T; SS.pin_have[0][kt] = 1;
        if (!g_det_fast) forward_peer(0, T, 8, SS.nlocal > 0 ? SS.nlocal : 1);
    }
    else {
        wbuf w = {SS.last_sinput, 0, sizeof SS.last_sinput}; int count = T - SS.base + 1 < 8 ? (int)(T - SS.base) + 1 : 8;
        w32(&w, T); w8(&w, (uint32_t)count); w8(&w, (uint32_t)SS.nlocal);
        for (int c = 0; c < count; c++) for (int l = 0; l < SS.nlocal; l++) w32(&w, SS.my_in[(T - (uint32_t)c) % SRING][l]);
        w8(&w, SS.epoch);
        SS.last_sinput_n = w.n;
        if (!g_det_fast && !SS.resync) send_to(&host_addr, NM_SINPUT, SS.last_sinput, w.n);   /* (catching up: these fields are long past) */
        SS.last_input_sent = ms();
    }
    {   static long bad = -2; if (bad == -2) { const char *e = getenv("RT_NET_TEST_DESYNC"); bad = e ? atol(e) : -1; }   /* testing: make this PC differ */
        if (bad >= 0 && F == (uint32_t)bad) W8(0x80001F00, R8(0x80001F00) ^ 0x5A); }
    if (F % 120 == 0 && F) {                          /* the state check */
        uint64_t h = det_state_hash();
        SS.hashes[F / 120 % 64] = h; SS.hash_fr[F / 120 % 64] = F;
        if (SS.host) for (int pr = 1; pr < SS.npeers; pr++) session_check(pr, F);
        {   static int lg = -1; if (lg < 0) lg = getenv("RT_NET_LOG_HASH") != NULL; if (lg) rt_log("net", "session: field %u hash %016llX", F, (unsigned long long)h); }
        if (!SS.host) { uint8_t b[12]; wbuf w = {b, 0, 12}; w32(&w, F); w64(&w, h); send_to(&host_addr, NM_SHASH, b, w.n); }
    }
    /* wait for field F */
    uint64_t t0 = ms(); int waited = 0, lost_shown = 0;
    for (;;) {
        session_poll();
        if (!SS.on) return;
        uint32_t k = F % SRING;
        if (SS.resync) {                              /* (frozen until the host's game is here) */
            if (ms() - SS.host_heard > HOST_LOST_MS) game_end("lost the connection to the host");
            resync_frame(); plat_pump(); plat_idle_wait(1000000); continue; }
        if (SS.fr_have[k] && SS.fr_fr[k] == F) { memcpy(SS.cur_btn, SS.fr_btn[k], sizeof SS.cur_btn); SS.fr_have[k] = 0; SS.host_fields++; break; }
        if (!SS.host && direct_field(F, SS.cur_btn)) {
            memcpy(SS.ran_btn[k], SS.cur_btn, sizeof SS.cur_btn); SS.ran_fr[k] = F; SS.ran_have[k] = 1; SS.direct_fields++;
            break;
        }
        uint64_t now = ms();
        if (!SS.host) {
            if (now - SS.last_req > 100 && now - t0 > 50) {   /* (a field we missed: ask for it) */
                SS.last_req = now; uint8_t b[4]; wbuf w = {b, 0, 4}; w32(&w, F); send_to(&host_addr, NM_SFREQ, b, w.n); }
            if (now - SS.last_input_sent > 200 && SS.last_sinput_n && !SS.resync) { SS.last_input_sent = now; send_to(&host_addr, NM_SINPUT, SS.last_sinput, SS.last_sinput_n); }
            if (now - SS.host_heard > 2000) {
                char n[96]; snprintf(n, sizeof n, "Online: reconnecting to the host (%llu s)", (unsigned long long)((now - SS.host_heard) / 1000));
                plat_title_note(n); if (!lost_shown) { lost_shown = 1; set_status("Lost the host - reconnecting..."); }
            }
            if (now - SS.host_heard > HOST_LOST_MS) game_end("lost the connection to the host");
        } else if (now - t0 > 1000) {
            char n[96]; snprintf(n, sizeof n, "Online: waiting for the other players (%llu s)", (unsigned long long)((now - t0) / 1000)); plat_title_note(n);
        }
        if (!waited && now - t0 > 2000) { waited = 1; if (!lost_shown) set_status("Waiting for the other players..."); }
        plat_pump();
        plat_idle_wait(500000);
    }
    /* behind the others (just joined again, or was cut off a while): run unpaced until caught up,
       asking for the next fields before running out of them */
    g_det_fast = !SS.host && (int32_t)(SS.newest_seen - F) > 6;
    if (g_det_fast && ms() - SS.last_req > 60) {
        uint32_t want = F + 1;
        while (want - F < 200 && SS.fr_have[want % SRING] && SS.fr_fr[want % SRING] == want) want++;
        if (want - F < 200 && (int32_t)(SS.newest_seen - want) >= 0) {
            SS.last_req = ms(); uint8_t b[4]; wbuf w = {b, 0, 4}; w32(&w, want); send_to(&host_addr, NM_SFREQ, b, w.n); }
    }
    if ((waited || lost_shown) && !SS.desync) set_status("Online game");
    {   int away = 0; if (SS.host) for (int pr = 1; pr < SS.npeers; pr++) if (SS.absent[pr] || SS.gone[pr]) away = 1;
        char n[128]; snprintf(n, sizeof n, "%s - input delay %d ms%s%s", SS.desync ? "Online: OUT OF SYNC" : SS.host ? "Online (host)" : "Online", SS.delay * 1000 / 60,
                              away ? " - a player is away" : "", g_det_fast ? " - catching up" : "");
        plat_title_note(n); }
    {   static uint64_t wait_ms, since; wait_ms += ms() - t0;              /* (how long the others kept us waiting) */
        if (F - since >= 600) {
            rt_log("net", "session: field %u, waited %llu ms in the last %u fields (input delay %d%s)", F, (unsigned long long)wait_ms, F - (uint32_t)since, SS.delay,
                   SS.host ? "" : SS.direct_fields + SS.host_fields ? "" : "");
            if (!SS.host) rt_log("net", "session: %d fields on the direct path, %d from the host's", SS.direct_fields, SS.host_fields);
            wait_ms = 0; since = F; SS.direct_fields = SS.host_fields = 0; } }
}
