/* rt_remotes.c - the eight player slots and what plays in each: a real Wii Remote, the keyboard,
 * or nothing.
 *
 * Slots 1-4 are the game's four Wii Remotes (WPAD channels, rt_wiimote.c: a real remote's reports
 * pass straight through, the keyboard drives an emulated one); the game has no more Wii Remote
 * channels, so slots 5-8 are the module's GameCube controller ports (CHAN_GC_0-3, rt_si.c): the
 * remote's or the keyboard's buttons are turned into a GameCube controller's, the way the module
 * reads one (measured: GC A -> 2, B -> 1, X -> A, Y -> 1+B, Start -> +, Z -> - and shake, the pad
 * turned sideways like the remote's).
 *
 * A remote that connects takes the next free slot (the lowest one without the keyboard or another
 * remote); when it disconnects, its slot is free again. Remote positions aren't remembered - only
 * which remotes we know (paired; they reconnect with any button). Options > Controls (rt_menu.c)
 * can move things around for the session: Left/Right on a slot picks the keyboard, a remote or
 * none (swapping with the slot that had it); a remote set to none sits out (no light) until it
 * disconnects or is picked again. wiimotes.ini (next to settings.ini) keeps "keyboard = N" (the
 * keyboard's slot, which is remembered) and "known = <address>" per known remote. The keyboard
 * is always in exactly one slot (player 1 at first), so keyboard + 7 remotes works. While player
 * 1's slot has nothing connected, the keyboard drives player 1 too (the game's own menus listen
 * to player 1 only).
 *
 * Our menus (title list, Options, Controls) read player 1's device and the keyboard together;
 * the game gets each slot's own buttons, and players 2-8 nothing while our menus are up.
 *
 * Player lights: the module numbers its players in the order their controllers appear (its
 * dRemoconMng_c keeps player -> channel). A remote shows its player's number: players 1-4 one
 * light each (1000, 0100, 0010, 0001), players 5-8 two neighbouring lights (1100, 0110, 0011,
 * 1001). */
#include "rt.h"

#define NSLOT 8
enum { A_NONE, A_KEYBOARD, A_REMOTE, A_NET };   /* A_NET: a player online (rt_net.c), on the host */
typedef struct { int kind; uint64_t addr; } assign_t;
static assign_t A[NSLOT];
static int gc_shown[8] RT_STATE;             /* GC slots: the port reports a controller (in order, see gc_present) */
void remotes_state_loaded(void) {}            /* what belongs in each slot */
#define NKNOWN 16
static uint64_t K[NKNOWN];           /* every remote we know (paired or put in a slot) until it's forgotten */
static int nknown;
static void know(uint64_t a) {
    if (!a) return;
    for (int i = 0; i < nknown; i++) if (K[i] == a) return;
    if (nknown < NKNOWN) K[nknown++] = a;
}
static void unknow(uint64_t a) { for (int i = 0; i < nknown; i++) if (K[i] == a) { K[i] = K[--nknown]; return; } }

typedef struct {
    int dev;                         /* platform device id in this slot now, -1 = none */
    uint64_t addr;                   /* its Bluetooth address (0 = unknown) */
    int temp;                        /* a remote with no address, here for now (not saved) */
    uint8_t led_sent, rumble;        /* lights last sent (0xFF: none yet), rumble bit of its last output */
    uint32_t btn;                    /* GC slots: the remote's core buttons */
    uint8_t ax, ay, az, have_accel;  /* GC slots: last accelerometer reading */
    int shake;                       /* GC slots: polls of shake left */
} slot_t;
static slot_t S[NSLOT];

#define NFREE 8
typedef struct { int dev; uint64_t addr; uint8_t led_sent; } free_t;
static free_t F[NFREE];              /* connected remotes with no slot (set to none, or no slot free) */
static int nfree;
static int parked[NFREE], nparked;   /* devices set to none in Controls: they stay out until they disconnect */
static int is_parked(int dev) { for (int i = 0; i < nparked; i++) if (parked[i] == dev) return 1; return 0; }
static void park(int dev) { if (dev >= 0 && !is_parked(dev) && nparked < NFREE) parked[nparked++] = dev; }
static void unpark(int dev) { for (int i = 0; i < nparked; i++) if (parked[i] == dev) { parked[i] = parked[--nparked]; return; } }
static int inited;

/* ---- wiimotes.ini ---- */
static void ini_path(char *p, size_t n) { snprintf(p, n, "%s/wiimotes.ini", g_root); }
static int keyboard_slot(void) { for (int i = 0; i < NSLOT; i++) if (A[i].kind == A_KEYBOARD) return i; return -1; }
static void save_memory(void) {
    char p[1200]; ini_path(p, sizeof p);
    FILE *f = fopen(p, "w"); if (!f) { rt_log("remotes", "can't write %s", p); return; }
    fprintf(f, "# Controllers (Options > Controls). keyboard = the keyboard's player slot; known = the Wii\n"
               "# Remotes paired with this PC (a remote takes the next free slot when it connects).\n"
               "# Written by the game.\n");
    fprintf(f, "keyboard = %d\n", keyboard_slot() + 1);
    for (int i = 0; i < nknown; i++) fprintf(f, "known = %012llX\n", (unsigned long long)K[i]);
    fclose(f);
}
static void load_memory(void) {
    char p[1200]; ini_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    int resave = 0;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            int slot; char v[64];
            if (sscanf(line, " known = %63s", v) == 1) { know(strtoull(v, NULL, 16)); continue; }
            if (sscanf(line, " keyboard = %d", &slot) == 1) { if (slot >= 1 && slot <= NSLOT && keyboard_slot() < 0) A[slot - 1].kind = A_KEYBOARD; continue; }
            if (sscanf(line, " slot%d = %63s", &slot, v) != 2 || slot < 1 || slot > NSLOT) continue;   /* (older files: slots) */
            resave = 1;
            if (!strcmp(v, "keyboard")) { if (keyboard_slot() < 0) A[slot - 1].kind = A_KEYBOARD; }
            else if (strcmp(v, "none")) know(strtoull(v, NULL, 16));
        }
        fclose(f);
    }
    if (keyboard_slot() < 0) { A[0].kind = A_KEYBOARD; resave = f != NULL; }
    if (resave) save_memory();
    rt_log("remotes", "keyboard: player slot %d; %d known Wii Remote%s", keyboard_slot() + 1, nknown, nknown == 1 ? "" : "s");
}
static void init(void) {
    if (inited) return;
    inited = 1;
    for (int i = 0; i < NSLOT; i++) { S[i].dev = -1; S[i].led_sent = 0xFF; }
    load_memory();
}

/* ---- players and lights ---- */
/* the module's player number (0-7) of the controller in slot s, -1 = not registered (yet) */
static int module_player(int s) {
    uint32_t rm = R32(0x8042A028u);                         /* dRemoconMng_c::m_instance: +4 connection per player */
    if (!rm) return -1;
    for (int p = 0; p < 8; p++) { uint32_t con = R32(rm + 4 + 4 * p); if (con && R32(con + 12) == (uint32_t)s) return p; }   /* CHAN_0-3 = 0-3, CHAN_GC_0-3 = 4-7 */
    return -1;
}
int remotes_player_of_slot(int s) { return s >= 0 && s < NSLOT ? module_player(s) : -1; }
uint8_t remotes_lights_for(int player) {                    /* bit 0 = light 1 */
    static const uint8_t pat[8] = {0x1, 0x2, 0x4, 0x8, 0x3, 0x6, 0xC, 0x9};
    return pat[player & 7];
}
static uint8_t slot_lights(int s) { int p = module_player(s); return remotes_lights_for(p >= 0 ? p : s); }
static void send_lights_to(int dev, uint8_t *sent, uint8_t rumble, uint8_t l) {
    if (l == *sent) return;
    uint8_t r[2] = {0x11, (uint8_t)((l << 4) | rumble)};
    plat_wm_write(dev, r, 2);
    *sent = l;
}

/* ---- devices coming and going ---- */
static int slot_of_dev(int dev) { for (int i = 0; i < NSLOT; i++) if (S[i].dev == dev) return i; return -1; }
static int slot_of_addr(uint64_t a) { for (int i = 0; i < NSLOT; i++) if (A[i].kind == A_REMOTE && A[i].addr == a) return i; return -1; }
static int free_slot(void) { for (int i = 0; i < NSLOT; i++) if (A[i].kind == A_NONE && S[i].dev < 0) return i; return -1; }
static int known(uint64_t a) { for (int i = 0; i < nknown; i++) if (K[i] == a) return 1; return 0; }
static void put_in_slot(int s, int dev, uint64_t addr, int temp) {
    memset(&S[s], 0, sizeof S[s]);
    S[s].dev = dev; S[s].addr = addr; S[s].temp = temp; S[s].led_sent = 0xFF; S[s].ax = S[s].ay = 0x80; S[s].az = 0x9A;
    rt_log("remotes", "Wii Remote %012llX -> player slot %d (%s)", (unsigned long long)addr, s + 1, s < 4 ? "Wii Remote channel" : "GameCube port");
    if (s >= 4 || session_on()) { uint8_t r[3] = {0x12, 0x04, 0x31}; plat_wm_write(dev, r, 3); }   /* ours to drive: buttons + motion, continuous */
}
static void take_out(int s, const char *why) {
    rt_log("remotes", "Wii Remote %012llX out of player slot %d (%s)", (unsigned long long)S[s].addr, s + 1, why);
    if (!strcmp(why, "disconnected") && A[s].kind == A_REMOTE && A[s].addr == S[s].addr) { A[s].kind = A_NONE; A[s].addr = 0; }   /* (its slot is free again) */
    S[s].dev = -1; gc_shown[s] = 0; S[s].temp = 0; S[s].btn = 0; S[s].shake = 0;
}

/* GC slots: the remote's reports -> buttons and shake */
static void pump_gc(int s) {
    uint8_t r[23]; int n;
    while ((n = plat_wm_read(S[s].dev, r, sizeof r)) > 0) {
        if (n >= 3 && ((r[0] >= 0x20 && r[0] <= 0x22) || (r[0] >= 0x30 && r[0] <= 0x37))) S[s].btn = (r[1] & 0x1F) | ((uint32_t)(r[2] & 0x9F) << 8);
        if (r[0] == 0x20) { uint8_t q[3] = {0x12, 0x04, 0x31}; plat_wm_write(S[s].dev, q, 3); S[s].led_sent = 0xFF; }   /* (a status report resets the mode) */
        if ((r[0] == 0x31 || r[0] == 0x33 || r[0] == 0x35 || r[0] == 0x37) && n >= 6) {
            int d = abs(r[3] - S[s].ax) + abs(r[4] - S[s].ay) + abs(r[5] - S[s].az);
            if (S[s].have_accel && d > 90) S[s].shake = 12;  /* a sharp swing: a shake */
            S[s].ax = r[3]; S[s].ay = r[4]; S[s].az = r[5]; S[s].have_accel = 1;
        }
    }
}

/* a remote paired from the Controls screen: known from now on (it takes a slot when it connects) */
static void adopt_paired(void) {
    static unsigned seen;
    unsigned gen = 0; uint64_t a = plat_wm_pair_last(&gen);
    if (gen == seen) return;
    seen = gen;
    if (a && !known(a)) { know(a); save_memory(); rt_log("remotes", "paired %012llX: known", (unsigned long long)a); }
}

static uint64_t last_scan;
void remotes_update(void) {
    init();
    net_update();                                           /* (online play: rt_net.c) */
    uint64_t now = rt_now_ns();
    if (!last_scan || now - last_scan >= 100000000ull) {    /* 10 times a second */
        last_scan = now;
        adopt_paired();
        plat_wm_dev devs[16]; int n = plat_wm_list(devs, 16);
        for (int s = 0; s < NSLOT; s++) if (S[s].dev >= 0) {
            int still = 0; for (int k = 0; k < n; k++) if (devs[k].id == S[s].dev) still = 1;
            if (!still) take_out(s, "disconnected");
            else if (S[s].temp ? A[s].kind != A_NONE : !(A[s].kind == A_REMOTE && A[s].addr == S[s].addr)) take_out(s, "moved");
        }
        for (int i = 0; i < nparked; i++) {                 /* (parked ones that left: forget the parking) */
            int still = 0; for (int k = 0; k < n; k++) if (devs[k].id == parked[i]) still = 1;
            if (!still) { parked[i] = parked[--nparked]; i--; }
        }
        free_t old[NFREE]; int nold = nfree; memcpy(old, F, sizeof F);
        nfree = 0;
        for (int k = 0; k < n; k++) {
            if (slot_of_dev(devs[k].id) >= 0) continue;
            int s = devs[k].addr ? slot_of_addr(devs[k].addr) : -1;
            if (s >= 0 && S[s].dev < 0) { put_in_slot(s, devs[k].id, devs[k].addr, 0); continue; }   /* (put there in Controls) */
            if (s < 0 && !is_parked(devs[k].id) && (s = free_slot()) >= 0) {      /* the next free slot */
                if (devs[k].addr) {
                    A[s].kind = A_REMOTE; A[s].addr = devs[k].addr;
                    if (!known(devs[k].addr)) { know(devs[k].addr); save_memory(); }
                    put_in_slot(s, devs[k].id, devs[k].addr, 0);
                } else put_in_slot(s, devs[k].id, 0, 1);    /* (no address: can't be known) */
                continue;
            }
            if (nfree < NFREE) {                            /* connected, but no slot: not used */
                uint8_t led = 0xFF;
                for (int j = 0; j < nold; j++) if (old[j].dev == devs[k].id) led = old[j].led_sent;
                if (led == 0xFF) rt_log("remotes", "Wii Remote %012llX connected, no player slot (%s)", (unsigned long long)devs[k].addr, is_parked(devs[k].id) ? "set to none" : "all taken");
                F[nfree].dev = devs[k].id; F[nfree].addr = devs[k].addr; F[nfree].led_sent = led;
                nfree++;
            }
        }
        for (int s = 0; s < NSLOT; s++) if (S[s].dev >= 0) send_lights_to(S[s].dev, &S[s].led_sent, S[s].rumble, slot_lights(s));
        for (int k = 0; k < nfree; k++) {                   /* no slot: no light (and keep its reports from piling up) */
            send_lights_to(F[k].dev, &F[k].led_sent, 0, 0);
            uint8_t r[23]; while (plat_wm_read(F[k].dev, r, sizeof r) > 0) {}
        }
    }
    for (int s = session_on() ? 0 : 4; s < NSLOT; s++) if (S[s].dev >= 0) pump_gc(s);   /* (online game: all of ours; their buttons go online) */
}

/* An online game (rt_net.c session): the game's players are the session's (its player map), all
   played through emulated remotes with the buttons everyone agreed on for the field; this PC's own
   remotes and keyboard only feed this PC's players' buttons into the session. */
int remotes_slot_dev(int s) { init(); return s >= 0 && s < NSLOT && !session_on() ? S[s].dev : -1; }
int remotes_slot_keyboard(int s) { init(); return s >= 0 && s < NSLOT && A[s].kind == A_KEYBOARD; }
/* the slot is played without a real remote of its own: the keyboard, or an online player */
int remotes_slot_emulated(int s) { init(); if (session_on()) return session_slot_active(s); return s >= 0 && s < NSLOT && (A[s].kind == A_KEYBOARD || A[s].kind == A_NET); }

/* ---- online (rt_net.c) ---- */
static uint32_t dev_btn[NSLOT];      /* the last buttons of each slot's own remote (for sending them online) */
/* host: free player slots for an online player's controllers; returns how many it got */
int remotes_net_take(int count, int *slots) {
    init();
    int got = 0;
    for (int i = 0; i < NSLOT && got < count; i++)
        if (A[i].kind == A_NONE && S[i].dev < 0) { A[i].kind = A_NET; A[i].addr = 0; slots[got++] = i; rt_log("remotes", "player slot %d: online", i + 1); }
    for (int k = got; k < count; k++) slots[k] = -1;
    return got;
}
void remotes_net_release(int s) { init(); if (s >= 0 && s < NSLOT && A[s].kind == A_NET) { A[s].kind = A_NONE; rt_log("remotes", "player slot %d: free again", s + 1); } }
/* client: this PC's players (the keyboard and its connected remotes, in slot order) */
static int local_slot(int p) {
    int k = 0;
    for (int i = 0; i < NSLOT; i++) if (A[i].kind == A_KEYBOARD || S[i].dev >= 0) { if (k == p) return i; k++; }
    return -1;
}
int remotes_local_slot(int p) { init(); return local_slot(p); }
/* the buttons of slot s's own controller now (menus that each controller drives: Who's playing?) */
uint32_t remotes_slot_raw(int s) {
    init();
    if (s < 0 || s >= NSLOT) return 0;
    uint32_t b = 0;
    if (A[s].kind == A_KEYBOARD || (s == 0 && S[0].dev < 0 && keyboard_slot() < 0)) b |= plat_buttons();
    if (S[s].dev >= 0) b |= s < 4 ? dev_btn[s] : S[s].btn;
    return b;
}
int remotes_local_players(void) { init(); int n = 0; while (local_slot(n) >= 0) n++; return n ? n : 1; }
uint32_t remotes_local_buttons(int p) {
    init();
    int s = local_slot(p);
    if (s < 0) return p == 0 ? plat_buttons() : 0;            /* (nothing set up: the keyboard plays player 1) */
    if (session_on()) {                                     /* (online game: its remotes are pumped here) */
        uint32_t b = A[s].kind == A_KEYBOARD ? plat_buttons() : 0;
        if (S[s].dev >= 0) b |= S[s].btn | (S[s].shake > 0 ? 0x10000u : 0);
        if (S[s].shake > 0) S[s].shake--;
        return b;
    }
    if (A[s].kind == A_KEYBOARD) return plat_buttons() | (S[s].dev >= 0 ? dev_btn[s] : 0);
    return s >= 4 ? S[s].btn : dev_btn[s];
}
int remotes_read(int dev, uint8_t *r, int max) { return plat_wm_read(dev, r, max); }
void remotes_write(int dev, const uint8_t *r, int len) {
    int s = slot_of_dev(dev);
    uint8_t b[23]; if (len > 23) len = 23; memcpy(b, r, len);
    if (s >= 0 && len >= 2) {
        S[s].rumble = b[1] & 1;
        if (b[0] == 0x11) { uint8_t l = slot_lights(s); b[1] = (uint8_t)((b[1] & 0x0F) | (l << 4)); S[s].led_sent = l; }   /* our player lights */
    }
    plat_wm_write(dev, b, len);
}

/* ---- buttons for the game ---- */
static uint32_t menu_out RT_STATE;   /* player 1 + keyboard, through our menus' filter (the game sees it: in a save state) */
/* the buttons slot s's game controller sends: its device's (`device`: a real remote's core
   buttons, 0 if none) plus the keyboard's if the keyboard is in this slot. Slot 0 (asked every
   update) runs our menus' filter on player 1's buttons together with the keyboard's. */
static uint32_t game_buttons(int s, uint32_t device);
uint32_t remotes_game_buttons(int s, uint32_t device) {   /* (RT_BTN_TRACE: logs each slot's buttons when they change) */
    static int tr = -1; static uint32_t last[NSLOT];
    if (s >= 0 && s < NSLOT) dev_btn[s] = device;
    if (tr < 0) tr = getenv("RT_BTN_TRACE") != NULL;
    uint32_t b = game_buttons(s, device);
    if (tr && s >= 0 && s < NSLOT && b != last[s]) { rt_log("remotes", "slot %d buttons %05X (frame %llu)", s + 1, b, (unsigned long long)g_frames_presented); last[s] = b; }
    return b;
}
static uint32_t game_buttons(int s, uint32_t device) {
    init();
    if (s < 0 || s >= NSLOT) return 0;
    if (session_on()) {                                     /* online game: everyone's agreed buttons; our menus run on */
        uint32_t b = session_slot_buttons(s);               /* player 1's on every PC alike, so they stay the same too */
        if (s == 0) return menu_out = menu_filter_buttons(b);
        return menu_owns_input() ? 0 : b;
    }
    uint32_t kb = g_det ? det_keyboard() : plat_buttons();   /* (deterministic: as latched for this field) */
    int kb_here = A[s].kind == A_KEYBOARD || (s == 0 && S[0].dev < 0);   /* (player 1 with nothing connected: the keyboard) */
    uint32_t mine = device | (kb_here ? kb : 0);
    if (s == 0) {
        menu_out = menu_filter_buttons(mine | kb);
        return menu_out & ~(kb & ~mine);                    /* (our menus' own presses, e.g. the title's "2", go to player 1) */
    }
    if (menu_owns_input()) return 0;
    if (A[s].kind == A_NET) return net_slot_buttons(s);
    return kb_here ? (menu_out & kb) | device : device;     /* (the keyboard: the same "held since the menu" filter) */
}

/* ---- GameCube ports (slots 5-8) ---- */
enum { GC_LEFT = 0x0001, GC_RIGHT = 0x0002, GC_DOWN = 0x0004, GC_UP = 0x0008, GC_Z = 0x0010,
       GC_A = 0x0100, GC_B = 0x0200, GC_X = 0x0400, GC_Y = 0x0800, GC_START = 0x1000 };
enum { RM_LEFT = 0x0001, RM_RIGHT = 0x0002, RM_DOWN = 0x0004, RM_UP = 0x0008, RM_PLUS = 0x0010,
       RM_2 = 0x0100, RM_1 = 0x0200, RM_B = 0x0400, RM_A = 0x0800, RM_MINUS = 0x1000, RM_SHAKE = 0x10000 };
static uint16_t to_gc(uint32_t b, int shake) {
    uint16_t g = 0;
    if (b & RM_2) g |= GC_A;
    if (b & RM_1) g |= GC_B;
    if (b & RM_A) g |= GC_X;
    if (b & RM_B) g |= GC_Y;
    if (b & RM_PLUS) g |= GC_START;
    if ((b & (RM_MINUS | RM_SHAKE)) || shake) g |= GC_Z;
    /* the remote is held sideways; the module turns the GC pad the same way (GC left -> remote up) */
    if (b & RM_UP) g |= GC_LEFT;
    if (b & RM_DOWN) g |= GC_RIGHT;
    if (b & RM_LEFT) g |= GC_DOWN;
    if (b & RM_RIGHT) g |= GC_UP;
    return g;
}
static int slot_in_use(int s) { if (session_on()) return session_slot_active(s); return S[s].dev >= 0 || A[s].kind == A_KEYBOARD || A[s].kind == A_NET; }
/* a GC port shows its controller only some time after the controllers before it are in the
   game (the module registers a player a few frames after its controller appears), so the module
   numbers the players in slot order when they're all there from the start */
static uint64_t in_since[NSLOT] RT_STATE;   /* frame the slot's controller appeared in the game (0: not) */
static int settled(int s) { return in_since[s] && g_frames_presented - in_since[s] >= 30; }
static int gc_present(int port) {
    int s = 4 + port;
    for (int i = 0; i < 4; i++) {
        if (!wiimote_connected(i)) in_since[i] = 0;
        else if (!in_since[i]) in_since[i] = g_frames_presented;
    }
    if (!slot_in_use(s)) { in_since[s] = 0; gc_shown[s] = 0; return 0; }
    if (gc_shown[s]) return 1;
    for (int i = 0; i < 4; i++) if ((i == 0 || slot_in_use(i)) && !settled(i)) return 0;   /* (remote 1 is always there) */
    for (int p = 0; p < port; p++) if (slot_in_use(4 + p) && !settled(4 + p)) return 0;
    gc_shown[s] = 1; in_since[s] = g_frames_presented;
    return 1;
}
/* testing: RT_GC_TEST="frame:buttons hex:frames,..." puts a controller in GC port 0 holding those
   GC buttons (A 0100, B 0200, X 0400, Y 0800, Start 1000, Z 0010, pad 0001-0008) */
static int gc_test_state(int port, uint8_t s[8]) {
    static const char *spec; static int init_t;
    if (!init_t) { init_t = 1; spec = getenv("RT_GC_TEST"); }
    if (!spec || port != 0) return 0;
    uint32_t b = 0; const char *p = spec;
    while (*p) {
        long at = strtol(p, (char **)&p, 10); if (*p != ':') break; p++;
        uint32_t m = (uint32_t)strtoul(p, (char **)&p, 16); long dur = 10;
        if (*p == ':') { p++; dur = strtol(p, (char **)&p, 10); }
        if ((long)g_frames_presented >= at && (long)g_frames_presented < at + dur) b |= m;
        if (*p == ',') p++; else break;
    }
    s[0] = (uint8_t)(b >> 8); s[1] = (uint8_t)b; s[2] = s[3] = s[4] = s[5] = 0x80; s[6] = s[7] = 0x1F;
    return 1;
}
int remotes_gc_state(int port, uint8_t st[8]) {
    init();
    if (port < 0 || port > 3) return 0;
    if (gc_test_state(port, st)) return 1;
    if (!gc_present(port)) return 0;
    int s = 4 + port;
    if (S[s].shake > 0 && !session_on()) S[s].shake--;      /* (polled once per field) */
    uint32_t b = remotes_game_buttons(s, S[s].dev >= 0 && !session_on() ? S[s].btn : 0);
    uint16_t g = to_gc(b, !session_on() && S[s].dev >= 0 && S[s].shake > 0 && !menu_owns_input());
    st[0] = (uint8_t)(g >> 8); st[1] = (uint8_t)g;
    st[2] = st[3] = st[4] = st[5] = 0x80;                   /* sticks centred */
    st[6] = st[7] = 0x1F;                                   /* triggers released */
    return 1;
}

/* ---- for the Controls screen ---- */
/* what slot s holds: kind (REMOTES_NONE / _KEYBOARD / _REMOTE), the remote's address, whether
   it's connected now, the module's player number (-1: not in the game) */
void remotes_slot_info(int s, int *kind, uint64_t *addr, int *connected, int *player) {
    init();
    *kind = S[s].temp ? A_REMOTE : A[s].kind;
    *addr = A[s].kind == A_REMOTE ? A[s].addr : S[s].addr;
    *connected = S[s].dev >= 0 || A[s].kind == A_KEYBOARD || A[s].kind == A_NET;
    *player = module_player(s);
}
/* is a remote with this address connected now (in a slot or not)? */
int remotes_addr_connected(uint64_t a) {
    for (int s = 0; s < NSLOT; s++) if (S[s].dev >= 0 && S[s].addr == a) return 1;
    for (int k = 0; k < nfree; k++) if (F[k].addr == a) return 1;
    return 0;
}
/* connected remotes that have no slot */
int remotes_unassigned(uint64_t *addrs, int max) { init(); int n = 0; for (int k = 0; k < nfree && n < max; k++) addrs[n++] = F[k].addr; return n; }
/* the choices for a slot, in order: none, keyboard, every known remote (in a slot, or connected
   without one) */
int remotes_choices(int *kinds, uint64_t *addrs, int max) {
    init();
    int n = 0;
    if (n < max) { kinds[n] = A_NONE; addrs[n++] = 0; }
    if (n < max) { kinds[n] = A_KEYBOARD; addrs[n++] = 0; }
    for (int i = 0; i < nknown && n < max; i++) { kinds[n] = A_REMOTE; addrs[n++] = K[i]; }
    for (int k = 0; k < nfree && n < max; k++) if (F[k].addr) {
        int dup = 0; for (int i = 0; i < nknown; i++) if (K[i] == F[k].addr) dup = 1;
        if (!dup) { kinds[n] = A_REMOTE; addrs[n++] = F[k].addr; }
    }
    for (int i = 3; i < n; i++)                             /* the remotes by address: the same order whatever slot they're in */
        for (int j = i; j > 2 && addrs[j - 1] > addrs[j]; j--) { uint64_t t = addrs[j]; addrs[j] = addrs[j - 1]; addrs[j - 1] = t; }
    return n;
}
/* put a device (kind/addr) in slot s; the slot that had it gets s's old device (a swap). The
   keyboard can't be left out: a slot holding it can only swap it away. 1 = changed */
int remotes_assign(int s, int kind, uint64_t addr) {
    init();
    if (s < 0 || s >= NSLOT) return 0;
    if (A[s].kind == kind && (kind != A_REMOTE || A[s].addr == addr)) return 0;
    if (A[s].kind == A_NET) return 0;                                   /* (an online player's slot) */
    if (kind == A_NONE && A[s].kind == A_KEYBOARD) return 0;
    int from = -1;
    if (kind != A_NONE) for (int i = 0; i < NSLOT; i++) if (i != s && A[i].kind == kind && (kind != A_REMOTE || A[i].addr == addr)) from = i;
    if (from < 0 && A[s].kind == A_KEYBOARD)
        for (int i = 0; i < NSLOT; i++) if (i != s && A[i].kind == A_NONE && S[i].dev < 0) { from = i; break; }   /* (prefer a slot nothing's in) */
    if (from < 0 && A[s].kind == A_KEYBOARD) {              /* the keyboard gives way: to the first empty slot */
        for (int i = 0; i < NSLOT && from < 0; i++) if (i != s && A[i].kind == A_NONE) from = i;
        if (from < 0) return 0;
    }
    assign_t old = A[s];
    if (old.kind == A_REMOTE && from < 0 && S[s].dev >= 0) park(S[s].dev);   /* (the remote it had sits out) */
    if (kind == A_NONE && S[s].temp) park(S[s].dev);
    if (kind == A_REMOTE) for (int k = 0; k < nfree; k++) if (F[k].addr == addr) unpark(F[k].dev);
    A[s].kind = kind; A[s].addr = kind == A_REMOTE ? addr : 0;
    if (kind == A_REMOTE) know(addr);
    if (from >= 0) A[from] = old;
    save_memory();
    rt_log("remotes", "player slot %d: %s%s", s + 1, kind == A_KEYBOARD ? "keyboard" : kind == A_REMOTE ? "Wii Remote" : "none",
           from >= 0 ? " (swapped)" : "");
    last_scan = 0;                                          /* sort the remotes again now */
    return 1;
}
/* forget slot s's remote: its slot and the PC's pairing */
void remotes_forget(int s) {
    init();
    if (s < 0 || s >= NSLOT || (A[s].kind != A_REMOTE && !S[s].temp)) return;
    uint64_t a = A[s].kind == A_REMOTE ? A[s].addr : S[s].addr;
    A[s].kind = A_NONE; A[s].addr = 0; unknow(a); save_memory();
    if (a) plat_wm_forget(a);
    rt_log("remotes", "player slot %d: Wii Remote %012llX forgotten", s + 1, (unsigned long long)a);
    last_scan = 0;
}
