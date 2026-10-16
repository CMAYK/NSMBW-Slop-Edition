/* rt_update.c - updates from the author's server (docs/UPDATES.md).
 *
 * At start a background thread fetches the update list (latest.txt, made and signed by
 * tools/publish_update.py) from the address built into the game (update_url.txt, or url = ...
 * in <game>/update.ini). The list must carry a good Ed25519 signature from the key built into
 * the game (update_key.pub), else it is ignored. If its version is higher than ours, every
 * listed file whose SHA-256 differs from ours is downloaded to update/files/<sha256> (checked
 * against the list's hash), while the boot waits on the update screen (rt_menu.c) after
 * "hold the Wii Remote sideways".
 *
 * Installing: the running exe can't be overwritten but can be renamed, so nsmbw.exe becomes
 * nsmbw.exe.old and the new one takes its name; the other files are listed in
 * update/pending.txt and the game restarts. The new copy puts them in place first thing at
 * start (update_install_pending), before anything opens them, then deletes the .old files.
 *
 * Never written by an update: disc/, nand/, online/, update/, settings.ini, wiimotes.ini,
 * online.ini, profiles.json, update.ini. Not part of a save state (no RT_STATE_FILE). */
#include "rt.h"
#include "rt_version.h"
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <direct.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ version
   (not RT_VERSION: windows.h has that name - a resource type, 16 - and it silently replaced the
   string; an array initialised from the macro fails to compile if that ever happens again) */
static const char game_version[] = GAME_VERSION, game_update_url[] = GAME_UPDATE_URL, game_update_key[] = GAME_UPDATE_KEY;
const char *rt_version(void) { return game_version; }
int rt_version_cmp(const char *a, const char *b) {
    while (*a || *b) {
        long x = 0, y = 0;
        while (*a && *a != '.') { if (isdigit((unsigned char)*a)) x = x * 10 + (*a - '0'); a++; }
        while (*b && *b != '.') { if (isdigit((unsigned char)*b)) y = y * 10 + (*b - '0'); b++; }
        if (x != y) return x < y ? -1 : 1;
        if (*a) a++;
        if (*b) b++;
    }
    return 0;
}

/* ------------------------------------------------------------------ shared state */
#ifdef _WIN32
static CRITICAL_SECTION mu;
#define LOCK() EnterCriticalSection(&mu)
#define UNLOCK() LeaveCriticalSection(&mu)
#else
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&mu)
#define UNLOCK() pthread_mutex_unlock(&mu)
#endif
static volatile int st = UPD_IDLE, skip_req, started;
static char st_msg[256], new_ver[32];
static volatile int64_t got_bytes, total_bytes;
static int screen_on, screen_finished;
static char url[1024];
static uint8_t key[32]; static int have_key;

static void set_state(int s, const char *fmt, ...) {
    char m[256]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    LOCK(); snprintf(st_msg, sizeof st_msg, "%s", m); st = s; UNLOCK();
    rt_log("update", "%s", m);
}
int update_status(char *msg, int cap, int64_t *got, int64_t *total, double *bps, char *ver, int vcap) {
    static uint64_t t_last; static int64_t b_last; static double speed;
    LOCK();
    int s = st;
    if (msg) snprintf(msg, cap, "%s", st_msg);
    if (ver) snprintf(ver, vcap, "%s", new_ver);
    int64_t g = got_bytes, t = total_bytes;
    UNLOCK();
    if (got) *got = g;
    if (total) *total = t;
    uint64_t now = rt_now_ns();
    if (!t_last || g < b_last) { t_last = now; b_last = g; }
    else if (now - t_last > 1000000000ull) {
        double inst = (double)(g - b_last) * 1e9 / (double)(now - t_last);
        speed = speed > 0 ? speed * 0.5 + inst * 0.5 : inst;
        t_last = now; b_last = g;
    }
    if (bps) *bps = speed;
    return s;
}
void update_skip(void) { if (st == UPD_DOWNLOADING) { skip_req = 1; rt_log("update", "download skipped by the player"); } }
/* called every frame while the boot asks for the title (rt_lyt.c): 1 = not yet. The screen
   (rt_menu.c draws it) stays up while checking / downloading; a result stays a moment. */
static double scr_t0, scr_tstate, scr_tend; static int scr_last = -1;
int update_boot_hold(void) {
    if (screen_finished) return 0;
    if (st == UPD_OFF) { screen_finished = 1; return 0; }
    double now = rt_now_ns() / 1e9;
    if (!screen_on) { screen_on = 1; scr_t0 = now; rt_log("update", "boot waits on the update screen"); }
    int s = st;
    if (s != scr_last) { scr_last = s; scr_tstate = now; }
    double in_state = now - scr_tstate, shown = now - scr_t0;
    int end = 0;
    switch (s) {
    case UPD_LATEST: end = shown > 1.4 && in_state > 0.8; break;
    case UPD_NO_SERVER: end = shown > 1.4 && in_state > 1.6; break;
    case UPD_FAILED: end = in_state > 3.5; break;
    case UPD_SKIPPED: case UPD_OFF: end = 1; break;
    case UPD_RESTART: if (in_state > 0.8) { rt_log("update", "restarting into the new version"); plat_restart(); } break;
    default: break;
    }
    if (end) { screen_finished = 1; scr_tend = now; rt_log("update", "update screen done: on to the title"); return 0; }
    return 1;
}
int update_screen_active(void) { return screen_on && !screen_finished; }
void update_screen_times(double *t0, double *t_end) { *t0 = scr_t0; *t_end = scr_tend; }

/* ------------------------------------------------------------------ files */
static int fexists(const char *p) { struct stat s; return stat(p, &s) == 0; }
static int64_t fsize(const char *p) { struct stat s; return stat(p, &s) == 0 ? (int64_t)s.st_size : -1; }
static int make_dir(const char *p) {
#ifdef _WIN32
    return _mkdir(p) == 0 || errno == EEXIST;
#else
    return mkdir(p, 0755) == 0 || errno == EEXIST;
#endif
}
static void make_dirs_for(const char *file) {           /* the folders a file goes in */
    char d[1400]; snprintf(d, sizeof d, "%s", file);
    for (char *p = d + 1; *p; p++) if (*p == '/' || *p == '\\') { char c = *p; *p = 0; make_dir(d); *p = c; }
}
static int move_file(const char *from, const char *to) {
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from, to) == 0;
#endif
}
static int hash_file(const char *p, uint8_t out[32]) {
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    static uint8_t buf[1 << 16]; sha256_ctx c; sha256_init(&c); size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, n);
    fclose(f); sha256_final(&c, out); return 1;
}
static void hex32(const uint8_t *h, char *s) { for (int i = 0; i < 32; i++) sprintf(s + 2 * i, "%02x", h[i]); }
static int unhex(const char *s, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) { unsigned v; if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) || sscanf(s + 2 * i, "%2x", &v) != 1) return 0; out[i] = (uint8_t)v; }
    return 1;
}
/* a path from the list: relative, no "..", nothing the player owns */
static int safe_path(const char *p) {
    size_t n = strlen(p);
    if (!n || n > 300 || p[0] == '/' || strchr(p, '\\') || strchr(p, ':')) return 0;
    for (const char *c = p; *c; c++) if ((unsigned char)*c < 0x20) return 0;
    const char *s = p;
    while (*s) {                                            /* each part: not "", ".", ".." */
        const char *e = strchr(s, '/'); size_t l = e ? (size_t)(e - s) : strlen(s);
        if (!l || (l == 1 && s[0] == '.') || (l == 2 && s[0] == '.' && s[1] == '.')) return 0;
        s += l; if (*s) s++;
    }
    static const char *dirs[] = {"disc/", "nand/", "online/", "update/", "frames/", NULL};
    static const char *files[] = {"settings.ini", "wiimotes.ini", "online.ini", "profiles.json", "update.ini", NULL};
    char low[320]; for (size_t i = 0; i <= n; i++) low[i] = (char)tolower((unsigned char)p[i]);
    for (int i = 0; dirs[i]; i++) if (!strncmp(low, dirs[i], strlen(dirs[i]))) return 0;
    for (int i = 0; files[i]; i++) if (!strcmp(low, files[i])) return 0;
    if (n > 4 && (!strcmp(low + n - 4, ".old") || (n > 5 && !strcmp(low + n - 5, ".part")))) return 0;
    return 1;
}
static int is_exe(const char *rel) {
#ifdef _WIN32
    return !strcmp(rel, "nsmbw.exe");
#else
    (void)rel; return 0;
#endif
}
static void target_path(const char *rel, char *out, size_t cap) {
#ifdef _WIN32
    if (is_exe(rel)) { GetModuleFileNameA(NULL, out, (DWORD)cap); return; }   /* (the running exe, wherever it is) */
#endif
    snprintf(out, cap, "%s/%s", g_root, rel);
}
static void staged_path(const char *hash, char *out, size_t cap) { snprintf(out, cap, "%s/update/files/%s", g_root, hash); }

/* ------------------------------------------------------------------ HTTP GET (blocking; worker thread) */
typedef int (*http_sink)(void *ud, const void *p, size_t n);   /* 0 = stop */
#ifdef _WIN32
static int http_get(const char *u, int64_t from, int64_t to, http_sink sink, void *ud, int timeout_ms, char *err, int ecap) {
    wchar_t wurl[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, u, -1, wurl, 2048)) { snprintf(err, ecap, "bad address"); return 0; }
    URL_COMPONENTS uc; memset(&uc, 0, sizeof uc); uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || !uc.lpszHostName || uc.dwHostNameLength >= 256) { snprintf(err, ecap, "bad address %s", u); return 0; }
    wchar_t host[256], path[2048];
    memcpy(host, uc.lpszHostName, uc.dwHostNameLength * sizeof(wchar_t)); host[uc.dwHostNameLength] = 0;
    size_t pl = uc.dwUrlPathLength + uc.dwExtraInfoLength; if (pl >= 2048) { snprintf(err, ecap, "address too long"); return 0; }
    if (uc.dwUrlPathLength) memcpy(path, uc.lpszUrlPath, uc.dwUrlPathLength * sizeof(wchar_t));
    if (uc.dwExtraInfoLength) memcpy(path + uc.dwUrlPathLength, uc.lpszExtraInfo, uc.dwExtraInfoLength * sizeof(wchar_t));
    path[pl] = 0; if (!pl) { path[0] = L'/'; path[1] = 0; }
    HINTERNET s = WinHttpOpen(L"nsmbw-updater/1", 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (8.1+) */, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"nsmbw-updater/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { snprintf(err, ecap, "WinHttpOpen failed (%lu)", GetLastError()); return 0; }
    WinHttpSetTimeouts(s, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    int ok = 0; HINTERNET c = NULL, r = NULL;
    c = WinHttpConnect(s, host, uc.nPort, 0);
    if (c) r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!r) snprintf(err, ecap, "can't connect (%lu)", GetLastError());
    else {
        wchar_t hdr[160];
        if (from >= 0) swprintf(hdr, 160, L"Cache-Control: no-cache\r\nRange: bytes=%lld-%lld\r\n", (long long)from, (long long)to);
        else swprintf(hdr, 160, L"Cache-Control: no-cache\r\n");
        if (!WinHttpSendRequest(r, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r, NULL)) {
            snprintf(err, ecap, "no answer (error %lu)", GetLastError()); goto done; }
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        if (from >= 0 && code == 200) snprintf(err, ecap, "the server can't send part of a file (no HTTP ranges)");
        else if (code != (from >= 0 ? 206u : 200u)) snprintf(err, ecap, "HTTP %lu", (unsigned long)code);
        else {
            static uint8_t buf[1 << 16]; ok = 1;
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(r, buf, sizeof buf, &got)) { snprintf(err, ecap, "download broke off (error %lu)", GetLastError()); ok = 0; break; }
                if (!got) break;
                if (!sink(ud, buf, got)) { snprintf(err, ecap, "stopped"); ok = 0; break; }
            }
        }
    }
done:
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
#else
/* Linux (testing): curl */
static int http_get(const char *u, int64_t from, int64_t to, http_sink sink, void *ud, int timeout_ms, char *err, int ecap) {
    if (strchr(u, '\'')) { snprintf(err, ecap, "bad address"); return 0; }
    char cmd[2400], range[64] = "";
    if (from >= 0) snprintf(range, sizeof range, "-r %lld-%lld ", (long long)from, (long long)to);
    snprintf(cmd, sizeof cmd, "curl -sS -f -L %s--connect-timeout %d --speed-limit 1 --speed-time %d -H 'Cache-Control: no-cache' '%s' 2>/dev/null",
             range, (timeout_ms + 999) / 1000, (timeout_ms + 999) / 1000 + 5, u);
    FILE *p = popen(cmd, "r");
    if (!p) { snprintf(err, ecap, "can't run curl"); return 0; }
    static uint8_t buf[1 << 16]; size_t n; int stopped = 0;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) if (!sink(ud, buf, n)) { stopped = 1; break; }
    int rc = pclose(p);
    if (stopped) { snprintf(err, ecap, "stopped"); return 0; }
    if (rc) { snprintf(err, ecap, "curl failed (%d)", rc); return 0; }
    return 1;
}
#endif

/* ------------------------------------------------------------------ the list */
typedef struct { char hash[65]; int64_t size, off, csize; int method; char path[304]; int need; } ufile;
static ufile *F; static int nf;
static char zip_name[128]; static int64_t zip_size;
typedef struct { char *p; size_t n, cap, limit; int progress; } membuf;
static int mem_sink(void *ud, const void *p, size_t n) {
    membuf *m = ud;
    if (m->progress && skip_req) return 0;
    if (m->n + n > m->limit) return 0;                      /* (more than asked for) */
    if (m->n + n + 1 > m->cap) { m->cap = (m->n + n + 1) * 2; m->p = realloc(m->p, m->cap); }
    memcpy(m->p + m->n, p, n); m->n += n; m->p[m->n] = 0;
    if (m->progress) {
        LOCK(); got_bytes += (int64_t)n; UNLOCK();
        static long thr = -1; if (thr < 0) { const char *e = getenv("RT_UPDATE_THROTTLE"); thr = e ? atol(e) : 0; }   /* (testing: bytes/s) */
        if (thr > 0) {
#ifdef _WIN32
            Sleep((DWORD)(n * 1000 / (size_t)thr));
#else
            usleep((useconds_t)(n * 1000000ull / (size_t)thr));
#endif
        }
    }
    return 1;
}
static int safe_name(const char *z) {                      /* the zip's name: a plain file name */
    if (!*z || strlen(z) >= sizeof zip_name) return 0;
    for (const char *c = z; *c; c++) if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-' && *c != '_') return 0;
    return strcmp(z, ".") && strcmp(z, "..");
}
/* "nsmbw-update 2", "version X", ["notes ..."], "zip <size> <name>",
   "file <sha256> <size> <offset> <compressed size> <method> <path>"... (offset: the entry's data
   in the zip; method 0 stored / 8 deflate), then "sig <hex>" signing every byte before that line */
static int parse_list(char *text, size_t n, char *ver, int vcap, char *why, int wcap) {
    char *sig = NULL;
    for (char *p = text; p < text + n; ) {                  /* the last line starting "sig " */
        if (!strncmp(p, "sig ", 4)) sig = p;
        char *e = memchr(p, '\n', (size_t)(text + n - p)); if (!e) break; p = e + 1;
    }
    if (!sig) { snprintf(why, wcap, "the update list has no signature"); return 0; }
    uint8_t s[64];
    if (!unhex(sig + 4, s, 64)) { snprintf(why, wcap, "the update list's signature is damaged"); return 0; }
    if (!ed25519_verify(s, text, (size_t)(sig - text), key)) { snprintf(why, wcap, "the update list isn't signed with this game's key"); return 0; }
    *sig = 0;
    int fmt = 0; ver[0] = 0; nf = 0; free(F); F = NULL; int cap = 0; zip_name[0] = 0; zip_size = 0;
    for (char *line = text; line && *line; ) {
        char *e = strchr(line, '\n'); if (e) *e = 0;
        size_t l = strlen(line); if (l && line[l - 1] == '\r') line[--l] = 0;
        if (!strncmp(line, "nsmbw-update ", 13)) fmt = atoi(line + 13);
        else if (!strncmp(line, "version ", 8)) snprintf(ver, vcap, "%s", line + 8);
        else if (!strncmp(line, "zip ", 4)) {
            long long sz; int off = 0;
            if (sscanf(line + 4, "%lld %n", &sz, &off) < 1 || !off || sz <= 0 || !safe_name(line + 4 + off)) { snprintf(why, wcap, "the update list is damaged"); return 0; }
            zip_size = sz; snprintf(zip_name, sizeof zip_name, "%s", line + 4 + off);
        } else if (!strncmp(line, "file ", 5)) {
            char h[65]; long long sz, zo, cs; int method, off = 0;
            if (sscanf(line + 5, "%64s %lld %lld %lld %d %n", h, &sz, &zo, &cs, &method, &off) < 5 || !off || strlen(h) != 64 ||
                sz < 0 || zo < 0 || cs < 0 || (method != 0 && method != 8) || (method == 0 && cs != sz) || sz > 0x7FFFFFFF || cs > 0x7FFFFFFF) {
                snprintf(why, wcap, "the update list is damaged"); return 0; }
            const char *path = line + 5 + off;
            if (!safe_path(path)) { snprintf(why, wcap, "the update list names a file it may not change (%s)", path); return 0; }
            if (nf == cap) { cap = cap ? cap * 2 : 64; F = realloc(F, (size_t)cap * sizeof *F); }
            ufile *f = &F[nf++]; memset(f, 0, sizeof *f);
            for (int i = 0; i < 64; i++) f->hash[i] = (char)tolower((unsigned char)h[i]);
            f->size = sz; f->off = zo; f->csize = cs; f->method = method; snprintf(f->path, sizeof f->path, "%s", path);
        }
        line = e ? e + 1 : NULL;
    }
    if (fmt != 2) { snprintf(why, wcap, fmt > 2 ? "this update has to be downloaded by hand (a newer kind of update list)" : "the update list is damaged"); return 0; }
    if (!ver[0] || !zip_name[0]) { snprintf(why, wcap, "the update list is damaged"); return 0; }
    for (int i = 0; i < nf; i++) if (F[i].off + F[i].csize > zip_size) { snprintf(why, wcap, "the update list is damaged"); return 0; }
    return 1;
}
/* the folder of latest.txt + name [+ the list address's ?query] */
static void url_next_to_list(const char *name, char *out, size_t cap) {
    char base[1024]; snprintf(base, sizeof base, "%s", url);
    char *q = strchr(base, '?'); char query[512] = "";
    if (q) { snprintf(query, sizeof query, "%s", q); *q = 0; }
    char *sl = strrchr(base, '/'); if (sl) sl[1] = 0;
    snprintf(out, cap, "%s%s%s", base, name, query);
}
static void list_url(char *out, size_t cap) {          /* a folder address gets latest.txt */
    char base[1024]; snprintf(base, sizeof base, "%s", url);
    char *q = strchr(base, '?'); char query[512] = "";
    if (q) { snprintf(query, sizeof query, "%s", q); *q = 0; }
    size_t l = strlen(base);
    snprintf(out, cap, "%s%s%s", base, l && base[l - 1] == '/' ? "latest.txt" : "", query);
}

/* ------------------------------------------------------------------ download
   One file = its entry in the zipped build (an HTTP range), unpacked and checked against the
   list's SHA-256, then kept in update/files/<sha256>. */
char *stbi_zlib_decode_noheader_malloc(const char *buffer, int len, int *outlen);   /* (stb_image.h, rt_hdtex.c) */
static int download(ufile *f, char *why, int wcap) {
    char dst[1400], part[1410], u[2048], err[200] = "";
    staged_path(f->hash, dst, sizeof dst);
    snprintf(part, sizeof part, "%s.part", dst);
    if (fsize(dst) == f->size) {                            /* (got it on an earlier try) */
        uint8_t h[32]; char hx[65];
        if (hash_file(dst, h)) { hex32(h, hx); if (!strcmp(hx, f->hash)) { LOCK(); got_bytes += f->csize; UNLOCK(); return 1; } }
    }
    make_dirs_for(dst);
    url_next_to_list(zip_name, u, sizeof u);
    int64_t before = got_bytes;
    membuf m = {0}; m.limit = (size_t)f->csize; m.progress = 1;
    int ok = f->csize == 0 || http_get(u, f->off, f->off + f->csize - 1, mem_sink, &m, 15000, err, sizeof err);
    if (!ok || skip_req || (int64_t)m.n != f->csize) {
        free(m.p); LOCK(); got_bytes = before; UNLOCK();
        if (skip_req) snprintf(why, wcap, "skipped");
        else snprintf(why, wcap, "download of %s failed: %s", f->path, ok ? "it came down short" : err);
        return 0;
    }
    const char *data = m.p ? m.p : ""; int len = (int)f->size; char *unz = NULL;
    if (f->method == 8) {
        unz = stbi_zlib_decode_noheader_malloc(m.p, (int)m.n, &len);
        if (!unz) { free(m.p); snprintf(why, wcap, "%s came down damaged (try again later)", f->path); return 0; }
        data = unz;
    }
    uint8_t h[32]; char hx[65]; sha256_ctx c; sha256_init(&c); sha256_update(&c, data, (size_t)len); sha256_final(&c, h); hex32(h, hx);
    int good = len == f->size && !strcmp(hx, f->hash);
    FILE *o = good ? fopen(part, "wb") : NULL;
    int wrote = o && fwrite(data, 1, (size_t)len, o) == (size_t)len;
    if (o) fclose(o);
    free(unz); free(m.p);
    if (!good) { snprintf(why, wcap, "%s came down damaged (try again later)", f->path); return 0; }
    if (!wrote) { remove(part); snprintf(why, wcap, "can't write in the update folder"); return 0; }
    remove(dst);
    if (!move_file(part, dst)) { remove(part); snprintf(why, wcap, "can't write in the update folder"); return 0; }
    return 1;
}

/* ------------------------------------------------------------------ install */
static void old_name(const char *target, char *out, size_t cap) {
    snprintf(out, cap, "%s.old", target);
    for (int i = 2; fexists(out) && remove(out) != 0 && i < 10; i++) snprintf(out, cap, "%s.old%d", target, i);   /* (an older one still running) */
}
static void note_cleanup(const char *path) {
    char p[1200]; snprintf(p, sizeof p, "%s/update/cleanup.txt", g_root);
    FILE *f = fopen(p, "a"); if (f) { fprintf(f, "%s\n", path); fclose(f); }
}
/* put one staged file in place, keeping the old one as .old until it worked */
static int install_one(const char *hash, const char *rel, int keep_old) {
    char staged[1400], target[1400], old[1420];
    staged_path(hash, staged, sizeof staged); target_path(rel, target, sizeof target);
    if (!fexists(staged)) return 1;                         /* (done already) */
    make_dirs_for(target);
    int had = fexists(target);
    if (had) { old_name(target, old, sizeof old); if (!move_file(target, old)) { rt_log("update", "can't move %s aside (in use?)", target); return 0; } }
    if (!move_file(staged, target)) {
        rt_log("update", "can't put %s in place", target);
        if (had) move_file(old, target);
        return 0;
    }
    if (had) { if (keep_old || remove(old) != 0) note_cleanup(old); }
    rt_log("update", "installed %s", rel);
    return 1;
}
/* first thing at start: the files the last run downloaded (update/pending.txt) */
void update_install_pending(void) {
    char p[1200]; snprintf(p, sizeof p, "%s/update/pending.txt", g_root);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[512], left[64][400]; int nleft = 0, n = 0;
    while (fgets(line, sizeof line, f)) {
        char h[65]; int off = 0; size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (sscanf(line, "%64s %n", h, &off) < 1 || !off || strlen(h) != 64 || !safe_path(line + off)) continue;
        n++;
        if (!install_one(h, line + off, 0) && nleft < 64) snprintf(left[nleft++], sizeof left[0], "%s", line);
    }
    fclose(f);
    if (nleft) {                                             /* (in use: next time) */
        if ((f = fopen(p, "w"))) { for (int i = 0; i < nleft; i++) fprintf(f, "%s\n", left[i]); fclose(f); }
        rt_log("update", "%d of %d updated files couldn't be put in place yet (next start)", nleft, n);
    } else { remove(p); rt_log("update", "update installed (%d files)", n); }
}
/* the .old files of the last update (the old exe: once that copy has exited) */
static void cleanup_old(void) {
    char p[1200]; snprintf(p, sizeof p, "%s/update/cleanup.txt", g_root);
    for (int tries = 0; tries < 20; tries++) {
        FILE *f = fopen(p, "r"); if (!f) return;
        char line[1400], left[16][1400]; int nleft = 0;
        while (fgets(line, sizeof line, f)) {
            size_t l = strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            if (!l) continue;
            if (fexists(line) && remove(line) != 0 && nleft < 16) snprintf(left[nleft++], sizeof left[0], "%s", line);
        }
        fclose(f);
        if (!nleft) { remove(p); return; }
        if ((f = fopen(p, "w"))) { for (int i = 0; i < nleft; i++) fprintf(f, "%s\n", left[i]); fclose(f); }
#ifdef _WIN32
        Sleep(500);
#else
        usleep(500000);
#endif
    }
}

/* ------------------------------------------------------------------ the check */
static void run(void) {
    char lu[1100], err[200] = "", why[300] = "", ver[32];
    list_url(lu, sizeof lu);
    set_state(UPD_CHECKING, "Checking for updates...");
    membuf m = {0}; m.limit = 1u << 20;                      /* (a list is small) */
    if (!http_get(lu, -1, -1, mem_sink, &m, 6000, err, sizeof err) || !m.n) {
        { char *q = strchr(lu, '?'); if (q) *q = 0; rt_log("update", "%s: %s", lu, err[0] ? err : "empty"); }   /* (no password in the log) */
        free(m.p); set_state(UPD_NO_SERVER, "Couldn't reach the update server"); return;
    }
    int ok = parse_list(m.p, m.n, ver, sizeof ver, why, sizeof why);
    free(m.p);
    if (!ok) { set_state(UPD_FAILED, "Update check failed: %s", why); return; }
    LOCK(); snprintf(new_ver, sizeof new_ver, "%s", ver); UNLOCK();
    if (rt_version_cmp(ver, game_version) <= 0) { set_state(UPD_LATEST, "You have the latest version"); rt_log("update", "server has %s, this is %s", ver, game_version); return; }
    int64_t need = 0; int nneed = 0;
    for (int i = 0; i < nf; i++) {                           /* which files differ from ours */
        char t[1400]; uint8_t h[32]; char hx[65] = "";
        target_path(F[i].path, t, sizeof t);
        if (fsize(t) == F[i].size && hash_file(t, h)) hex32(h, hx);
        F[i].need = strcmp(hx, F[i].hash) != 0;
        if (F[i].need) { need += F[i].csize; nneed++; }
    }
    rt_log("update", "version %s is out (this is %s): %d of %d files to download, %.1f MB (packed) from %s", ver, game_version, nneed, nf, need / 1e6, zip_name);
    if (!nneed) { set_state(UPD_LATEST, "You have the latest version"); return; }
    LOCK(); got_bytes = 0; total_bytes = need; UNLOCK();
    set_state(UPD_DOWNLOADING, "Downloading version %s", ver);
    for (int i = 0; i < nf; i++) {
        if (!F[i].need) continue;
        if (!download(&F[i], why, sizeof why)) {
            if (skip_req) set_state(UPD_SKIPPED, "Update skipped");
            else set_state(UPD_FAILED, "Update failed: %s", why);
            return;
        }
    }
    set_state(UPD_INSTALLING, "Installing version %s...", ver);
    {   /* the rest goes in at the next start (update_install_pending) */
        char p[1200]; snprintf(p, sizeof p, "%s/update/pending.txt", g_root);
        FILE *f = fopen(p, "w");
        if (!f) { set_state(UPD_FAILED, "Update failed: can't write in the update folder"); return; }
        for (int i = 0; i < nf; i++) if (F[i].need && !is_exe(F[i].path)) fprintf(f, "%s %s\n", F[i].hash, F[i].path);
        fclose(f);
    }
    for (int i = 0; i < nf; i++)                             /* the running exe: renamed, the new one in its place */
        if (F[i].need && is_exe(F[i].path) && !install_one(F[i].hash, F[i].path, 1)) {
            char p[1200]; snprintf(p, sizeof p, "%s/update/pending.txt", g_root); remove(p);
            set_state(UPD_FAILED, "Update failed: couldn't replace nsmbw.exe"); return;
        }
    set_state(UPD_RESTART, "Restarting...");
}
#ifdef _WIN32
static DWORD WINAPI worker(LPVOID a) { (void)a; cleanup_old(); if (st != UPD_OFF) run(); return 0; }
#else
static void *worker(void *a) { (void)a; cleanup_old(); if (st != UPD_OFF) run(); return NULL; }
#endif
void update_begin(void) {
    if (started) return;
    started = 1;
#ifdef _WIN32
    InitializeCriticalSection(&mu);
#endif
    snprintf(url, sizeof url, "%s", game_update_url);
    have_key = strlen(game_update_key) == 64 && unhex(game_update_key, key, 32);
    {   /* <game>/update.ini: url = ... (another server), key = ... (its key) */
        char p[1200]; snprintf(p, sizeof p, "%s/update.ini", g_root);
        FILE *f = fopen(p, "r"); char line[1200];
        while (f && fgets(line, sizeof line, f)) {
            char k[16], v[1100] = "";
            if (line[0] == ';' || line[0] == '#' || sscanf(line, " %15[a-z] = %1099[^\r\n]", k, v) < 1) continue;
            for (int l = (int)strlen(v); l > 0 && (v[l - 1] == ' ' || v[l - 1] == '\t'); ) v[--l] = 0;
            if (!strcmp(k, "url")) snprintf(url, sizeof url, "%s", v);
            if (!strcmp(k, "key")) have_key = strlen(v) == 64 && unhex(v, key, 32);
        }
        if (f) fclose(f);
    }
    const char *e = getenv("RT_UPDATE"), *off = NULL;
    if (e && *e == '0') off = "RT_UPDATE=0";
    else if (!(e && *e == '1') && getenv("RT_DETERMINISTIC")) off = "deterministic test run";
    else if (!rt_setting("update_check", 1)) off = "update_check = 0 in settings.ini";
    else if (!url[0]) off = "no update server in this build (update_url.txt)";
    else if (!have_key) off = "no update key in this build (update_key.pub)";
    if (off) { st = UPD_OFF; rt_log("update", "version %s; no update check: %s", game_version, off); }
    else { char shown[1024]; snprintf(shown, sizeof shown, "%s", url); char *q = strchr(shown, '?'); if (q) strcpy(q, "?..."); rt_log("update", "version %s; update server %s", game_version, shown); }
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL); if (t) CloseHandle(t); else st = UPD_OFF;
#else
    pthread_t t; if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t); else st = UPD_OFF;
#endif
}
