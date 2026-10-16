/* rt_profiles.c - player profiles (docs/PROFILES.md).
 *
 * A profile is a person, not a character: an ID (16 hex digits, made once and never changed,
 * unique to its username), a username of 1-4 letters / digits (unique on this PC; shown only in
 * multiplayer), a preferred character (preselected at the character select unless someone else
 * already has it) and stats - global (all save files) and per save file. Profiles are kept in
 * <game>/profiles.json, together with which profile each controller (the keyboard, each Wii
 * Remote by address) was last used with.
 *
 * Multiplayer > "Who's playing?" (rt_menu.c) picks a profile per connected controller; the
 * choice goes into the game for the player slots (rt_chars.c: chars_set_slot_profile - the
 * username and preferred character, in the save state, so an online game has everyone's).
 * Online, a joining PC sends its players' usernames and preferred characters (rt_net.c HELLO).
 *
 * Next (TODO.md): lives / coins / power-up of a multiplayer save file per profile ID instead of
 * per character; counting the stats. */
#include "rt.h"

#define MAXPROF 64
typedef struct {
    char id[17], name[5];
    char pref[24];                              /* character id (rt_chars.c roster), "" none */
    uint32_t cleared, deaths, coins;            /* global stats */
} profile_t;
static profile_t P[MAXPROF];
static int np, loaded;
static struct { char key[32]; char id[17]; } CM[32];   /* controller -> profile it was last used with */
static int ncm;

/* ---- a small JSON reader (objects, arrays, strings, numbers, true/false/null) ---- */
typedef struct { const char *s; int i, n, bad; } jr;
static void jws(jr *r) { while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\t' || r->s[r->i] == '\n' || r->s[r->i] == '\r')) r->i++; }
static int jch(jr *r, char c) { jws(r); if (r->i < r->n && r->s[r->i] == c) { r->i++; return 1; } return 0; }
static void jstr(jr *r, char *out, int cap) {
    jws(r); int k = 0;
    if (r->i >= r->n || r->s[r->i] != '"') { r->bad = 1; return; }
    r->i++;
    while (r->i < r->n && r->s[r->i] != '"') {
        char c = r->s[r->i++];
        if (c == '\\' && r->i < r->n) { c = r->s[r->i++]; if (c == 'u') { r->i += 4; c = '?'; } else if (c == 'n') c = '\n'; }
        if (k < cap - 1) out[k++] = c;
    }
    out[k] = 0; r->i++;
}
static double jnum(jr *r) { jws(r); char *e; double v = strtod(r->s + r->i, &e); if (e == r->s + r->i) r->bad = 1; r->i = (int)(e - r->s); return v; }
static void jskip(jr *r) {                      /* any value */
    jws(r); if (r->i >= r->n) { r->bad = 1; return; }
    char c = r->s[r->i];
    if (c == '"') { char t[2]; jstr(r, t, 2); }
    else if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']'; r->i++;
        if (jch(r, close)) return;
        do { if (c == '{') { char k[2]; jstr(r, k, 2); if (!jch(r, ':')) { r->bad = 1; return; } } jskip(r); } while (!r->bad && jch(r, ','));
        if (!jch(r, close)) r->bad = 1;
    } else if (!strncmp(r->s + r->i, "true", 4)) r->i += 4;
    else if (!strncmp(r->s + r->i, "false", 5)) r->i += 5;
    else if (!strncmp(r->s + r->i, "null", 4)) r->i += 4;
    else jnum(r);
}

static void path(char *p, size_t n) { snprintf(p, n, "%s/profiles.json", g_root); }
static int valid_name(const char *s) {
    int n = (int)strlen(s); if (n < 1 || n > 4) return 0;
    for (int i = 0; i < n; i++) if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return 0;
    return 1;
}
static void load(void) {
    if (loaded) return;
    loaded = 1;
    char p[1200]; path(p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1); if (fread(s, 1, n, f) != (size_t)n) n = 0; s[n] = 0; fclose(f);
    jr r = {s, 0, (int)n, 0};
    if (!jch(&r, '{')) { free(s); return; }
    if (!jch(&r, '}')) do {
        char key[32]; jstr(&r, key, sizeof key); if (!jch(&r, ':')) break;
        if (!strcmp(key, "profiles") && jch(&r, '[')) {
            if (!jch(&r, ']')) { do {
                profile_t pr; memset(&pr, 0, sizeof pr);
                if (!jch(&r, '{')) { r.bad = 1; break; }
                if (!jch(&r, '}')) { do {
                    char k[32]; jstr(&r, k, sizeof k); if (!jch(&r, ':')) { r.bad = 1; break; }
                    if (!strcmp(k, "id")) jstr(&r, pr.id, sizeof pr.id);
                    else if (!strcmp(k, "name")) jstr(&r, pr.name, sizeof pr.name);
                    else if (!strcmp(k, "preferred_character")) jstr(&r, pr.pref, sizeof pr.pref);
                    else if (!strcmp(k, "stats") && jch(&r, '{')) {
                        if (!jch(&r, '}')) { do {
                            char sk[32]; jstr(&r, sk, sizeof sk); if (!jch(&r, ':')) { r.bad = 1; break; }
                            if (!strcmp(sk, "levels_cleared")) pr.cleared = (uint32_t)jnum(&r);
                            else if (!strcmp(sk, "deaths")) pr.deaths = (uint32_t)jnum(&r);
                            else if (!strcmp(sk, "coins")) pr.coins = (uint32_t)jnum(&r);
                            else jskip(&r);
                        } while (!r.bad && jch(&r, ',')); if (!jch(&r, '}')) r.bad = 1; }
                    } else jskip(&r);
                } while (!r.bad && jch(&r, ',')); if (!jch(&r, '}')) r.bad = 1; }
                if (!r.bad && pr.id[0] && valid_name(pr.name) && np < MAXPROF) P[np++] = pr;
            } while (!r.bad && jch(&r, ',')); if (!jch(&r, ']')) r.bad = 1; }
        } else if (!strcmp(key, "controllers") && jch(&r, '{')) {
            if (!jch(&r, '}')) { do {
                char k[32], v[17]; jstr(&r, k, sizeof k); if (!jch(&r, ':')) { r.bad = 1; break; } jstr(&r, v, sizeof v);
                if (!r.bad && ncm < 32) { snprintf(CM[ncm].key, sizeof CM[ncm].key, "%s", k); snprintf(CM[ncm].id, sizeof CM[ncm].id, "%s", v); ncm++; }
            } while (!r.bad && jch(&r, ',')); if (!jch(&r, '}')) r.bad = 1; }
        } else jskip(&r);
    } while (!r.bad && jch(&r, ','));
    if (r.bad) rt_log("profiles", "profiles.json: can't read all of it (kept %d profiles)", np);
    else rt_log("profiles", "%d profile%s", np, np == 1 ? "" : "s");
    free(s);
}
static void save(void) {
    char p[1200], tmp[1300]; path(p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w"); if (!f) { rt_log("profiles", "can't write %s", tmp); return; }
    fprintf(f, "{\n  \"version\": 1,\n  \"profiles\": [");
    for (int i = 0; i < np; i++)
        fprintf(f, "%s\n    {\"id\": \"%s\", \"name\": \"%s\", \"preferred_character\": \"%s\",\n     \"stats\": {\"levels_cleared\": %u, \"deaths\": %u, \"coins\": %u}}",
                i ? "," : "", P[i].id, P[i].name, P[i].pref, P[i].cleared, P[i].deaths, P[i].coins);
    fprintf(f, "\n  ],\n  \"controllers\": {");
    for (int i = 0; i < ncm; i++) fprintf(f, "%s\n    \"%s\": \"%s\"", i ? "," : "", CM[i].key, CM[i].id);
    fprintf(f, "\n  }\n}\n");
    fclose(f);
    remove(p); rename(tmp, p);
}

/* ---- the API ---- */
int prof_count(void) { load(); return np; }
const char *prof_name(int i) { load(); return i >= 0 && i < np ? P[i].name : ""; }
const char *prof_id(int i) { load(); return i >= 0 && i < np ? P[i].id : ""; }
const char *prof_pref(int i) { load(); return i >= 0 && i < np ? P[i].pref : ""; }
void prof_stats(int i, uint32_t *cleared, uint32_t *deaths, uint32_t *coins) {
    load(); if (i < 0 || i >= np) { *cleared = *deaths = *coins = 0; return; }
    *cleared = P[i].cleared; *deaths = P[i].deaths; *coins = P[i].coins;
}
int prof_find_name(const char *name) { load(); for (int i = 0; i < np; i++) if (!strcmp(P[i].name, name)) return i; return -1; }
int prof_find_id(const char *id) { load(); for (int i = 0; i < np; i++) if (!strcmp(P[i].id, id)) return i; return -1; }
/* a new profile: its username must be free; returns its index (-1: bad / taken / full) */
int prof_create(const char *name, const char *pref) {
    load();
    if (!valid_name(name) || prof_find_name(name) >= 0 || np >= MAXPROF) return -1;
    profile_t *pr = &P[np]; memset(pr, 0, sizeof *pr);
    for (;;) {                                   /* a random ID no other profile here has */
        static uint64_t ctr; uint64_t x = rt_now_ns() ^ ((uint64_t)(uintptr_t)pr << 17) ^ (++ctr * 0x9E3779B97F4A7C15ull);
        x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 29; x *= 0x94D049BB133111EBull; x ^= x >> 32;
        snprintf(pr->id, sizeof pr->id, "%016llX", (unsigned long long)x);
        if (prof_find_id(pr->id) < 0) break;
    }
    snprintf(pr->name, sizeof pr->name, "%s", name);
    snprintf(pr->pref, sizeof pr->pref, "%s", pref ? pref : "");
    np++; save();
    rt_log("profiles", "new profile %s (%s)", pr->name, pr->id);
    return np - 1;
}
/* rename / change the preferred character (-1: the new name is bad or someone else's) */
int prof_edit(int i, const char *name, const char *pref) {
    load(); if (i < 0 || i >= np) return -1;
    if (!valid_name(name)) return -1;
    int other = prof_find_name(name); if (other >= 0 && other != i) return -1;
    snprintf(P[i].name, sizeof P[i].name, "%s", name);
    snprintf(P[i].pref, sizeof P[i].pref, "%s", pref ? pref : "");
    save(); return i;
}
/* the controller in player slot s: its key in the file ("keyboard", "remote:ADDR"; "" none) */
static void controller_key(int s, char *k, size_t n) {
    int kind, conn, player; uint64_t addr; remotes_slot_info(s, &kind, &addr, &conn, &player);
    if (kind == REMOTES_KEYBOARD || (s == 0 && !conn)) snprintf(k, n, "keyboard");
    else if (kind == REMOTES_REMOTE && addr) snprintf(k, n, "remote:%012llX", (unsigned long long)addr);
    else k[0] = 0;
}
int prof_of_controller(int s) {                  /* the profile this slot's controller was last used with (-1) */
    load(); char k[32]; controller_key(s, k, sizeof k); if (!k[0]) return -1;
    for (int i = 0; i < ncm; i++) if (!strcmp(CM[i].key, k)) return prof_find_id(CM[i].id);
    return -1;
}
void prof_set_controller(int s, int i) {        /* remember: this slot's controller plays as profile i */
    load(); char k[32]; controller_key(s, k, sizeof k); if (!k[0] || i < 0 || i >= np) return;
    for (int j = 0; j < ncm; j++) if (!strcmp(CM[j].key, k)) { snprintf(CM[j].id, sizeof CM[j].id, "%s", P[i].id); save(); return; }
    if (ncm < 32) { snprintf(CM[ncm].key, sizeof CM[ncm].key, "%s", k); snprintf(CM[ncm].id, sizeof CM[ncm].id, "%s", P[i].id); ncm++; save(); }
}

/* who plays in which slot this time (Who's playing?), and putting it into the game */
static int slot_prof[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
int prof_of_slot(int s) { return s >= 0 && s < 8 ? slot_prof[s] : -1; }
void prof_set_slot(int s, int i) {
    if (s < 0 || s >= 8) return;
    slot_prof[s] = i;
    if (!session_on()) chars_set_slot_profile(s, i >= 0 ? P[i].name : "", i >= 0 ? chars_index(P[i].pref) : -1);
}
/* this PC's players for an online HELLO: username + preferred character per local player */
void prof_local_player(int k, char name[5], int *pref) {
    int s = remotes_local_slot(k), i = s >= 0 ? prof_of_slot(s) : -1;
    snprintf(name, 5, "%s", i >= 0 ? P[i].name : "");
    *pref = i >= 0 ? chars_index(P[i].pref) : -1;
}
