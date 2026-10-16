/* rt_discord.c - Discord Rich Presence ("Playing New Super Mario Bros. Wii - World 1-2").
 *
 * Talks to the Discord app on this PC through its local IPC (no SDK, nothing to ship):
 * Windows: the named pipe \\.\pipe\discord-ipc-N, Linux: the socket <runtime dir>/discord-ipc-N
 * (N = 0..9). A frame is: opcode (u32 LE), length (u32 LE), JSON. We send HANDSHAKE (op 0,
 * {"v":1,"client_id":...}) once, then SET_ACTIVITY commands (op 1); Discord answers each.
 *
 * The game thread only builds the activity text (discord_frame(), from menu_draw); a background
 * thread owns the connection: it connects when Discord is running (retries every 15 s), sends
 * the newest activity, and reconnects if Discord restarts. Discord accepts about 5 updates per
 * 20 s, so changes are sent at most every 4 s (the last one always arrives).
 *
 * The application ID comes from discord_app_id.txt at build time (out/rt_version.h,
 * GAME_DISCORD_ID); without one, or with settings.ini discord_presence = 0, this does nothing.
 * The image keys (large "logo", small = the character's id, e.g. "mario") are the art assets
 * uploaded to that application. docs/DISCORD.md. Not part of save states (host-side only). */
#include "rt.h"
#include "rt_version.h"
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#endif

#ifndef GAME_DISCORD_ID
#define GAME_DISCORD_ID ""
#endif

/* ------------------------------------------------------------------ connection (own thread) */
#ifdef _WIN32
typedef HANDLE conn_t;
#define CONN_NONE INVALID_HANDLE_VALUE
static conn_t ipc_open(void) {
    for (int i = 0; i < 10; i++) {
        char name[64]; snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return CONN_NONE;
}
static int ipc_write(conn_t c, const void *p, int n) { DWORD w; return WriteFile(c, p, (DWORD)n, &w, NULL) && (int)w == n; }
static int ipc_read(conn_t c, void *p, int n) {
    for (int got = 0; got < n;) { DWORD r; if (!ReadFile(c, (char *)p + got, (DWORD)(n - got), &r, NULL) || !r) return 0; got += (int)r; }
    return 1;
}
static void ipc_close(conn_t c) { CloseHandle(c); }
static int my_pid(void) { return (int)GetCurrentProcessId(); }
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
typedef int conn_t;
#define CONN_NONE (-1)
static conn_t ipc_open(void) {
    const char *dirs[4] = { getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), "/tmp" };
    static const char *sub[3] = { "", "app/com.discordapp.Discord/", "snap.discord/" };   /* (flatpak, snap) */
    for (int d = 0; d < 4; d++) {
        if (!dirs[d] || !dirs[d][0]) continue;
        for (int s = 0; s < 3; s++) for (int i = 0; i < 10; i++) {
            struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
            snprintf(a.sun_path, sizeof a.sun_path, "%s/%sdiscord-ipc-%d", dirs[d], sub[s], i);
            int fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) return CONN_NONE;
            if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) return fd;
            close(fd);
        }
    }
    return CONN_NONE;
}
static int ipc_write(conn_t c, const void *p, int n) {
    for (int done = 0; done < n;) { ssize_t w = send(c, (const char *)p + done, (size_t)(n - done), MSG_NOSIGNAL); if (w <= 0) return 0; done += (int)w; }
    return 1;
}
static int ipc_read(conn_t c, void *p, int n) {
    for (int got = 0; got < n;) { ssize_t r = recv(c, (char *)p + got, (size_t)(n - got), 0); if (r <= 0) return 0; got += (int)r; }
    return 1;
}
static void ipc_close(conn_t c) { close(c); }
static int my_pid(void) { return (int)getpid(); }
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

/* one frame out, then the answer in (the body is only logged when it's an error) */
static int ipc_call(conn_t c, uint32_t op, const char *json, char *reply, int rmax) {
    uint32_t n = (uint32_t)strlen(json);
    uint8_t h[8] = { (uint8_t)op, (uint8_t)(op >> 8), (uint8_t)(op >> 16), (uint8_t)(op >> 24),
                     (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24) };
    if (!ipc_write(c, h, 8) || !ipc_write(c, json, (int)n)) return 0;
    uint8_t rh[8]; if (!ipc_read(c, rh, 8)) return 0;
    uint32_t rop = rh[0] | rh[1] << 8 | rh[2] << 16 | (uint32_t)rh[3] << 24, rn = rh[4] | rh[5] << 8 | rh[6] << 16 | (uint32_t)rh[7] << 24;
    if (rn > 1u << 20) return 0;
    char *body = malloc(rn + 1); if (!body) return 0;
    int ok = ipc_read(c, body, (int)rn); body[ok ? rn : 0] = 0;
    if (ok && reply) snprintf(reply, rmax, "%s", body);
    if (ok && (rop == 2 || strstr(body, "\"evt\":\"ERROR\""))) { rt_log("discord", "Discord says: %.200s", body); ok = rop != 2; }
    free(body);
    return ok;
}

/* shared between the game thread and the connection thread */
static volatile int pending_seq, sent_seq;
static char pending[2048];
#ifdef _WIN32
static CRITICAL_SECTION lock;
#define LOCK() EnterCriticalSection(&lock)
#define UNLOCK() LeaveCriticalSection(&lock)
#else
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
#endif

#ifdef _WIN32
static DWORD WINAPI conn_thread(LPVOID arg)
#else
static void *conn_thread(void *arg)
#endif
{
    (void)arg;
    conn_t c = CONN_NONE; int nonce = 0, said_missing = 0;
    for (;;) {
        if (c == CONN_NONE) {
            c = ipc_open();
            if (c == CONN_NONE) { if (!said_missing) { rt_log("discord", "Discord isn't running (looking again every 15 s)"); said_missing = 1; } sleep_ms(15000); continue; }
            char hello[160], reply[512] = "";
            snprintf(hello, sizeof hello, "{\"v\":1,\"client_id\":\"%s\"}", GAME_DISCORD_ID);
            if (!ipc_call(c, 0, hello, reply, sizeof reply)) { rt_log("discord", "Discord didn't accept the connection"); ipc_close(c); c = CONN_NONE; sleep_ms(15000); continue; }
            const char *u = strstr(reply, "\"username\":\""); char who[40] = "";
            if (u) { u += 12; int i = 0; while (u[i] && u[i] != '"' && i < 39) { who[i] = u[i]; i++; } who[i] = 0; }
            rt_log("discord", "connected to Discord%s%s", who[0] ? " as " : "", who);
            said_missing = 0; sent_seq = -1;
        }
        int seq = pending_seq;
        if (seq != sent_seq) {
            char act[2048]; LOCK(); memcpy(act, pending, sizeof act); UNLOCK();
            char cmd[2400];
            snprintf(cmd, sizeof cmd, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":%s},\"nonce\":\"%d\"}", my_pid(), act, ++nonce);
            if (!ipc_call(c, 1, cmd, NULL, 0)) { rt_log("discord", "lost the connection to Discord"); ipc_close(c); c = CONN_NONE; continue; }
            sent_seq = seq;
            sleep_ms(4000);                                   /* Discord's rate limit (5 per 20 s) */
        } else sleep_ms(250);
    }
    return 0;
}

/* ------------------------------------------------------------------ what we're doing (game thread) */
#define G_NOW_SCENE        0x80428732u
#define G_GAME_MODE        0x8042A204u      /* dScStage_c::m_gameMode (2 = title) */
#define G_STAGE_INSTANCE   0x8042A1C8u      /* dScStage_c::m_instance: +0x120C world, +0x120D stage */
#define G_WMAP_WORLD       0x8042A24Du      /* dScWMap_c::m_WorldNo */
#define MOD_PY_ENTRY       0x8185E890u      /* daPyMng_c::mPlayerEntry[8] */
enum { SC_WORLD_MAP = 3, SC_STAGE = 5, SC_OPENING = 8, SC_GAME_SETUP = 10 };
const char *chars_name(int c);
int chars_of_player(int n);

static void stage_name(int world, int stage, char *out, int n) {
    static const char *special[] = { [19] = "Coin Battle", [20] = "Ghost House", [21] = "Tower", [22] = "Tower", [23] = "Castle",
        [24] = "Castle", [25] = "Toad House", [26] = "Toad House", [27] = "Toad House", [28] = "Toad House", [32] = "Enemy Ambush",
        [33] = "Enemy Ambush", [34] = "Enemy Ambush", [35] = "Warp Cannon", [37] = "Airship", [38] = "Toad Rescue",
        [40] = "Peach's Castle", [41] = "Staff Roll" };
    if (stage >= 0 && stage < 19) snprintf(out, n, "World %d-%d", world + 1, stage + 1);
    else if (stage >= 0 && stage < 42 && special[stage]) {
        if (stage == 41 || stage == 40 || stage == 19) snprintf(out, n, "%s", special[stage]);
        else snprintf(out, n, "World %d - %s", world + 1, special[stage]);
    } else snprintf(out, n, "World %d", world + 1);
}

static void json_str(char *out, int n, const char *s) {      /* "s" with JSON escapes */
    int k = 0; if (k < n - 1) out[k++] = '"';
    for (; *s && k < n - 3; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[k++] = '\\'; out[k++] = (char)ch; }
        else if (ch >= 0x20) out[k++] = (char)ch;
    }
    out[k++] = '"'; out[k] = 0;
}

static int started, enabled = -1;
static int64_t start_time;

static void build(char *details, int dn, char *state, int sn, int *party) {
    int scene = R16(G_NOW_SCENE);
    uint32_t mode = R32(G_GAME_MODE);
    int players = 0; for (int i = 0; i < 8; i++) if (R32(MOD_PY_ENTRY + 4 * i)) players++;
    *party = players;
    if (scene == SC_STAGE && mode == 2) { snprintf(details, dn, "On the title screen"); *party = 0; }
    else if (scene == SC_GAME_SETUP) { snprintf(details, dn, "Getting ready"); *party = 0; }
    else if (scene == SC_OPENING) snprintf(details, dn, "Watching the opening");
    else if (scene == SC_WORLD_MAP) {
        int w = R8(G_WMAP_WORLD);
        if (w < 10) snprintf(details, dn, "World %d map", w + 1); else snprintf(details, dn, "On the world map");
    } else if (scene == SC_STAGE) {
        uint32_t st = R32(G_STAGE_INSTANCE);
        if (st >= 0x80000000u && st < 0x81800000u) stage_name(R8(st + 0x120C), R8(st + 0x120D), details, dn);
        else snprintf(details, dn, "Playing");
    } else { snprintf(details, dn, "Starting up"); *party = 0; }
    if (session_on() || net_role() == NET_CLIENT || net_role() == NET_HOST) {
        int n = net_online_players();
        snprintf(state, sn, n > 1 ? "Online, %d players" : "Online", n);
    } else if (*party > 1) snprintf(state, sn, "Multiplayer");
    else if (*party == 1) snprintf(state, sn, "Singleplayer");
    else state[0] = 0;
}

void discord_frame(void) {
    if (enabled < 0) {
        enabled = GAME_DISCORD_ID[0] && rt_setting("discord_presence", 1);
        if (!GAME_DISCORD_ID[0]) rt_log("discord", "rich presence off in this build (no discord_app_id.txt)");
        else if (!enabled) rt_log("discord", "rich presence off (settings.ini discord_presence = 0)");
    }
    if (!enabled) return;
    if (!started) {
        started = 1; start_time = (int64_t)time(NULL);
#ifdef _WIN32
        InitializeCriticalSection(&lock);
        HANDLE h = CreateThread(NULL, 0, conn_thread, NULL, 0, NULL); if (h) CloseHandle(h);
#else
        pthread_t t; if (!pthread_create(&t, NULL, conn_thread, NULL)) pthread_detach(t);
#endif
    }
    static uint64_t last_ns; uint64_t now = rt_now_ns();
    if (now - last_ns < 500000000ull) return;                 /* look twice a second */
    last_ns = now;
    char details[96], state[96]; int party;
    build(details, sizeof details, state, sizeof state, &party);
    int c = chars_of_player(0); const char *cname = c >= 0 ? chars_name(c) : NULL; const char *cid = c >= 0 ? chars_id(c) : NULL;
    char jd[200], js[200], jc[120], act[2048]; int k = 0;
    json_str(jd, sizeof jd, details); json_str(js, sizeof js, state);
    k += snprintf(act + k, sizeof act - k, "{\"details\":%s", jd);
    if (state[0]) k += snprintf(act + k, sizeof act - k, ",\"state\":%s", js);
    k += snprintf(act + k, sizeof act - k, ",\"timestamps\":{\"start\":%lld}", (long long)start_time);
    k += snprintf(act + k, sizeof act - k, ",\"assets\":{\"large_image\":\"logo\",\"large_text\":\"New Super Mario Bros. Wii\"");
    if (cid && party > 0) { json_str(jc, sizeof jc, cname); k += snprintf(act + k, sizeof act - k, ",\"small_image\":\"%s\",\"small_text\":%s", cid, jc); }
    k += snprintf(act + k, sizeof act - k, "}");
    if (party > 1) k += snprintf(act + k, sizeof act - k, ",\"party\":{\"id\":\"nsmbw-%d-%lld\",\"size\":[%d,8]}", my_pid(), (long long)start_time, party);
    k += snprintf(act + k, sizeof act - k, "}");
    LOCK();
    int changed = strcmp(act, pending) != 0;
    if (changed) { snprintf(pending, sizeof pending, "%s", act); pending_seq++; }
    UNLOCK();
    if (changed) LOG("discord", "presence: %s / %s", details, state);
}
