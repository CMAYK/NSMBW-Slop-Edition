/* rt_wiimote.c - the four Wii Remotes the game sees (WPAD channels 0-3, players' remotes 1-4
 * in rt_remotes.c's slots 0-3) on the emulated Bluetooth controller (rt_bt.c).
 *
 * Each is an emulated remote like Dolphin's: registered in SYSCONF (BT.DINF, patched at boot to
 * hold all four, rt_ios.c), it pages the host once page scan is on, then opens the HID control
 * (PSM 0x11) and interrupt (PSM 0x13) L2CAP channels itself and speaks the Wii Remote HID
 * protocol on them. Each is driven by what its player slot holds (rt_remotes.c): a real remote,
 * the keyboard (plat_buttons()) or nothing. Remote 0 is always connected (the title needs a
 * player 1); remotes 1-3 connect while their slot has a real remote or the keyboard, and
 * disconnect when it goes. A real remote's reports pass straight through
 * (Dolphin's "Real Wii Remote"): the game's output reports go to it (the player lights are put
 * in by rt_remotes.c), its input reports come back unchanged. */
#include "rt.h"
#include "rt_bt.h"
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

#define NWM 4
static const char wm_name[] = "Nintendo RVL-CNT-01";
enum { ST_IDLE, ST_REQUESTED, ST_CONNECTED };

typedef struct {
    int used;
    uint16_t psm, lcid, rcid;
    int our_cfg_sent, our_cfg_ok, their_cfg_ok;
} chan_t;

typedef struct {
    int idx; uint16_t handle; uint8_t addr[6];             /* HCI (LSB-first) order */
    int state; uint64_t t_state;
    chan_t ch[4]; uint8_t sig_ident;
    uint8_t leds, report_mode, continuous, ir_on, speaker_on;
    uint32_t last_btn; uint64_t t_last_report;
    struct { int active; uint32_t addr; int left; int space; } rd;   /* pending memory read */
    int dev;                                                /* real remote passed through (-1: none) */
    uint8_t saved_out[0x20][23]; int saved_len[0x20];      /* state-setting output reports, replayed on attach */
    uint8_t pend[64][23]; int pend_len[64], npend;          /* its input reports waiting for the host */
    uint32_t real_btn, game_btn;
    int announced, shake_phase, shake_left, shake_n;
    uint64_t in_frame;                                      /* frame its HID channels opened */
} wm_t;
static wm_t W[NWM];
static uint8_t eeprom[0x1700];
static int in_update;

#define LE16(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((v) >> 8); } while (0)
#define RD16(p) ((uint16_t)((p)[0] | ((p)[1] << 8)))

/* ---- L2CAP ---- */
static chan_t *chan_by_lcid(wm_t *w, uint16_t c) { for (int i = 0; i < 4; i++) if (w->ch[i].used && w->ch[i].lcid == c) return &w->ch[i]; return NULL; }
static chan_t *chan_by_psm(wm_t *w, uint16_t p) { for (int i = 0; i < 4; i++) if (w->ch[i].used && w->ch[i].psm == p) return &w->ch[i]; return NULL; }
static chan_t *chan_new(wm_t *w, uint16_t psm) {
    for (int i = 0; i < 4; i++) if (!w->ch[i].used) {
        memset(&w->ch[i], 0, sizeof w->ch[i]); w->ch[i].used = 1; w->ch[i].psm = psm; w->ch[i].lcid = 0x40 + i; return &w->ch[i];
    }
    return NULL;
}
static int chan_ready(chan_t *c) { return c && c->rcid && c->our_cfg_ok && c->their_cfg_ok; }

static void l2cap_send(wm_t *w, uint16_t cid, const uint8_t *d, int len) {
    uint8_t b[300];
    b[0] = len & 0xFF; b[1] = len >> 8; b[2] = cid & 0xFF; b[3] = cid >> 8;
    memcpy(b + 4, d, len);
    bt_send_acl(w->handle, b, len + 4);
}
static void sig_send(wm_t *w, uint8_t code, uint8_t ident, const uint8_t *d, int len) {
    uint8_t b[64]; b[0] = code; b[1] = ident; b[2] = len & 0xFF; b[3] = len >> 8;
    memcpy(b + 4, d, len);
    l2cap_send(w, 0x0001, b, len + 4);
}
static void send_conn_req(wm_t *w, chan_t *c) {
    uint8_t d[4]; LE16(d, c->psm); LE16(d + 2, c->lcid);
    LOG("wm", "remote %d: L2CAP connect request psm %02X", w->idx, c->psm);
    sig_send(w, 0x02, w->sig_ident++, d, 4);
}
static void send_cfg_req(wm_t *w, chan_t *c) {
    uint8_t d[8]; LE16(d, c->rcid); LE16(d + 2, 0); d[4] = 0x01; d[5] = 2; LE16(d + 6, 185);   /* MTU 185 */
    c->our_cfg_sent = 1;
    sig_send(w, 0x04, w->sig_ident++, d, 8);
}

/* ---- the emulated remote ---- */
static void eeprom_init(void) {
    static const uint8_t cal[10] = {0x80, 0x80, 0x80, 0x00, 0x9A, 0x9A, 0x9A, 0x00, 0x40, 0xE3};
    /* IR calibration (Dolphin defaults) at 0x00 and 0x0B */
    static const uint8_t ir[11] = {0x66, 0x6E, 0x77, 0xB8, 0x83, 0x71, 0x89, 0x88, 0x30, 0x00, 0x00};
    memcpy(eeprom + 0x00, ir, 11); memcpy(eeprom + 0x0B, ir, 11);
    memcpy(eeprom + 0x16, cal, 10); memcpy(eeprom + 0x20, cal, 10);
}
static void hid_send(wm_t *w, const uint8_t *d, int len) {   /* input report on the interrupt channel */
    chan_t *c = chan_by_psm(w, 0x13);
    if (!chan_ready(c)) return;
    uint8_t b[32]; b[0] = 0xA1; memcpy(b + 1, d, len);
    l2cap_send(w, c->rcid, b, len + 1);
}
static void put_buttons(uint8_t *p, uint32_t btn) { p[0] = btn & 0x1F; p[1] = (btn >> 8) & 0x9F; }
static void send_ack(wm_t *w, uint8_t rpt, uint8_t err) {
    uint8_t r[5] = {0x22}; put_buttons(r + 1, w->game_btn); r[3] = rpt; r[4] = err; hid_send(w, r, 5);
}
static void send_status(wm_t *w) {
    uint8_t r[7] = {0x20};
    put_buttons(r + 1, w->game_btn);
    r[3] = (w->speaker_on ? 0x04 : 0) | (w->ir_on ? 0x08 : 0) | (w->leds << 4);
    r[6] = 0xC0;                                            /* battery */
    hid_send(w, r, 7);
}
/* shake (keyboard): a strong back-and-forth swing that flips every second report, so the game
   sees the motion change from one frame to the next whatever the report timing. A tap shakes
   for at least 16 reports (~160 ms) so short presses register. Returns 1 while shaking (and
   sets the accelerometer bytes). Called once per report sent. */
static int kb_shake_accel(wm_t *w, uint32_t btn, uint8_t *ax, uint8_t *ay, uint8_t *az) {
    if (btn & 0x10000) { if (!w->shake_phase) { w->shake_phase = 1; w->shake_n = 0; } w->shake_left = 16; }
    else if (w->shake_phase && --w->shake_left <= 0) w->shake_phase = 0;
    if (!w->shake_phase) return 0;
    int ph = (w->shake_n++ >> 1) & 1;
    *ax = ph ? 0x10 : 0xF0; *ay = ph ? 0xF0 : 0x10; *az = ph ? 0x30 : 0xE0;
    return 1;
}
static void send_data_report(wm_t *w) {
    uint32_t btn = w->game_btn;
    uint8_t r[22]; memset(r, 0, sizeof r);
    r[0] = w->report_mode; put_buttons(r + 1, btn);
    uint8_t ax = 0x80, ay = 0x80, az = 0x9A;
    kb_shake_accel(w, btn, &ax, &ay, &az);
    int n;
    switch (w->report_mode) {
    case 0x30: n = 3; break;
    case 0x31: r[3] = ax; r[4] = ay; r[5] = az; n = 6; break;
    case 0x32: memset(r + 3, 0xFF, 8); n = 11; break;
    case 0x33: r[3] = ax; r[4] = ay; r[5] = az; memset(r + 6, 0xFF, 12); n = 18; break;
    case 0x34: memset(r + 3, 0xFF, 19); n = 22; break;
    case 0x35: r[3] = ax; r[4] = ay; r[5] = az; memset(r + 6, 0xFF, 16); n = 22; break;
    case 0x36: memset(r + 3, 0xFF, 19); n = 22; break;
    case 0x37: r[3] = ax; r[4] = ay; r[5] = az; memset(r + 6, 0xFF, 16); n = 22; break;
    case 0x3D: memset(r + 1, 0xFF, 21); n = 22; break;
    default: r[0] = 0x30; n = 3; break;
    }
    hid_send(w, r, n);
    w->last_btn = btn;
}
static void read_step(wm_t *w) {                             /* one 0x21 report of a pending read */
    uint8_t r[22]; memset(r, 0, sizeof r);
    r[0] = 0x21; put_buttons(r + 1, w->game_btn);
    int n = w->rd.left > 16 ? 16 : w->rd.left, err = 0;
    if (w->rd.space == 0) {                                 /* EEPROM */
        if (w->rd.addr + n > sizeof eeprom) err = 8;
        else memcpy(r + 6, eeprom + w->rd.addr, n);
    } else {                                                /* control registers */
        uint32_t reg = (w->rd.addr >> 16) & 0xFE;
        if (reg == 0xA4) err = 7;                           /* no extension connected */
    }
    r[3] = (uint8_t)(((n - 1) << 4) | err);
    r[4] = (w->rd.addr >> 8) & 0xFF; r[5] = w->rd.addr & 0xFF;
    hid_send(w, r, 22);
    if (err) { w->rd.active = 0; return; }
    w->rd.addr += n; w->rd.left -= n;
    if (w->rd.left <= 0) w->rd.active = 0;
}

/* ---- a real remote passed through ---- */
static int is_data_report(uint8_t id) { return id >= 0x30 && id <= 0x3F; }
static int has_buttons(const uint8_t *r, int n) { return n >= 3 && ((r[0] >= 0x20 && r[0] <= 0x22) || (r[0] >= 0x30 && r[0] <= 0x37)); }
static void merge_buttons(wm_t *w, uint8_t *r, int n) {     /* the buttons the game may see (menus, keyboard) */
    if (!has_buttons(r, n)) return;
    r[1] = (r[1] & ~0x1F) | (w->game_btn & 0x1F); r[2] = (r[2] & ~0x9F) | ((w->game_btn >> 8) & 0x9F);
    uint8_t ax, ay, az;                                     /* keyboard shake overrides the remote's motion */
    if ((r[0] == 0x31 || r[0] == 0x33 || r[0] == 0x35 || r[0] == 0x37) && n >= 6 && kb_shake_accel(w, w->game_btn, &ax, &ay, &az)) { r[3] = ax; r[4] = ay; r[5] = az; }
}
static void real_pump(wm_t *w) {
    uint8_t r[23]; int n;
    while ((n = remotes_read(w->dev, r, sizeof r)) > 0) {
        if (w->npend && is_data_report(r[0]) && is_data_report(w->pend[w->npend - 1][0])) w->npend--;   /* keep only the newest data report */
        if (w->npend == 64) { memmove(w->pend, w->pend + 1, sizeof w->pend[0] * 63); memmove(w->pend_len, w->pend_len + 1, sizeof w->pend_len[0] * 63); w->npend--; }
        memcpy(w->pend[w->npend], r, n); w->pend_len[w->npend++] = n;
        if (has_buttons(r, n)) w->real_btn = (r[1] & 0x1F) | ((uint32_t)(r[2] & 0x9F) << 8);
    }
    w->game_btn = remotes_game_buttons(w->idx, w->real_btn);    /* (+ the keyboard if it's this player's; our menus) */
    while (w->npend && bt_acl_queued() <= 2) {
        merge_buttons(w, w->pend[0], w->pend_len[0]);
        hid_send(w, w->pend[0], w->pend_len[0]);
        memmove(w->pend, w->pend + 1, sizeof w->pend[0] * (w->npend - 1)); memmove(w->pend_len, w->pend_len + 1, sizeof w->pend_len[0] * (w->npend - 1)); w->npend--;
    }
}
static void real_attach(wm_t *w, int dev) {
    w->dev = dev; w->rd.active = 0; w->npend = 0; w->real_btn = 0;
    uint8_t r[23]; while (remotes_read(dev, r, sizeof r) > 0) {}      /* drop what arrived before */
    static const uint8_t order[] = {0x11, 0x13, 0x1A, 0x14, 0x19, 0x12};   /* LEDs, IR, speaker, then reporting mode */
    for (size_t i = 0; i < sizeof order; i++) if (w->saved_len[order[i]]) remotes_write(dev, w->saved_out[order[i]], w->saved_len[order[i]]);
    uint8_t st[2] = {0x15, 0}; remotes_write(dev, st, 2);           /* status report: tells the game about extensions/battery */
    rt_log("wm", "remote %d: a real Wii Remote", w->idx);
}

static void hid_output(wm_t *w, const uint8_t *d, int len) {     /* d[0] = report id */
    if (len < 2) return;
    uint8_t id = d[0];
    int ack = d[1] & 0x02;
    if (id == 0x11 || id == 0x12 || id == 0x13 || id == 0x14 || id == 0x19 || id == 0x1A) { memcpy(w->saved_out[id], d, len > 23 ? 23 : len); w->saved_len[id] = len > 23 ? 23 : len; }
    switch (id) {                                           /* our view of the state (keyboard / emulation) */
    case 0x11: w->leds = d[1] >> 4; break;
    case 0x12: {
        uint8_t m = len > 2 ? d[2] : 0x30, c = (d[1] & 0x04) != 0;
        if (m != w->report_mode || c != w->continuous) LOG("wm", "remote %d: reporting mode %02X%s", w->idx, m, c ? " (continuous)" : "");
        w->continuous = c; w->report_mode = m; break;
    }
    case 0x13: case 0x1A: w->ir_on = (d[1] & 0x04) != 0; break;
    case 0x14: w->speaker_on = (d[1] & 0x04) != 0; break;
    }
    if (w->dev >= 0) { remotes_write(w->dev, d, len); return; }
    LOG("wm", "remote %d: HID out %02X %02X len %d", w->idx, id, d[1], len);
    switch (id) {
    case 0x10: break;                                       /* rumble */
    case 0x11: if (ack) send_ack(w, id, 0); break;
    case 0x12:
        if (ack) send_ack(w, id, 0);
        send_data_report(w);
        break;
    case 0x13: case 0x1A: case 0x14: case 0x19: if (ack) send_ack(w, id, 0); break;
    case 0x15: send_status(w); break;
    case 0x16: {                                            /* write memory / registers */
        uint32_t addr = (d[2] << 16) | (d[3] << 8) | d[4]; int n = d[5];
        if (!(d[1] & 0x04) && addr + n <= sizeof eeprom && len >= 6 + n) memcpy(eeprom + addr, d + 6, n);
        send_ack(w, id, 0);
        break;
    }
    case 0x17: {                                            /* read memory / registers */
        w->rd.space = (d[1] & 0x04) ? 1 : 0;
        w->rd.addr = (d[2] << 16) | (d[3] << 8) | d[4];
        w->rd.left = (d[5] << 8) | d[6];
        w->rd.active = w->rd.left > 0;
        if (w->rd.space == 0) w->rd.addr &= 0xFFFF;
        break;
    }
    case 0x18: break;                                       /* speaker data */
    default: rt_log("wm", "unhandled HID output report %02X", id); break;
    }
}

/* ---- L2CAP signalling from the host ---- */
static void sig_recv(wm_t *w, const uint8_t *p, int len) {
    while (len >= 4) {
        uint8_t code = p[0], ident = p[1]; uint16_t clen = RD16(p + 2);
        const uint8_t *d = p + 4;
        switch (code) {
        case 0x02: {                                        /* connection request (host -> us) */
            uint16_t psm = RD16(d), scid = RD16(d + 2);
            chan_t *c = chan_by_psm(w, psm); if (!c) c = chan_new(w, psm);
            uint8_t r[8];
            if (!c) { LE16(r, 0); LE16(r + 2, scid); LE16(r + 4, 4); LE16(r + 6, 0); sig_send(w, 0x03, ident, r, 8); break; }
            c->rcid = scid;
            LE16(r, c->lcid); LE16(r + 2, scid); LE16(r + 4, 0); LE16(r + 6, 0);
            sig_send(w, 0x03, ident, r, 8);
            send_cfg_req(w, c);
            break;
        }
        case 0x03: {                                        /* connection response */
            uint16_t dcid = RD16(d), scid = RD16(d + 2), result = RD16(d + 4);
            chan_t *c = chan_by_lcid(w, scid);
            if (c && result == 0) { c->rcid = dcid; if (!c->our_cfg_sent) send_cfg_req(w, c); }
            else if (c && result != 1) c->used = 0;
            break;
        }
        case 0x04: {                                        /* configuration request: accept as-is */
            uint16_t dcid = RD16(d);
            chan_t *c = chan_by_lcid(w, dcid);
            uint8_t r[64]; int olen = clen - 4; if (olen < 0) olen = 0; if (olen > 50) olen = 50;
            LE16(r, c ? c->rcid : 0); LE16(r + 2, 0); LE16(r + 4, 0);
            memcpy(r + 6, d + 4, olen);
            sig_send(w, 0x05, ident, r, 6 + olen);
            if (c) { c->their_cfg_ok = 1; if (!c->our_cfg_sent && c->rcid) send_cfg_req(w, c); }
            break;
        }
        case 0x05: {                                        /* configuration response */
            uint16_t scid = RD16(d), result = RD16(d + 4);
            chan_t *c = chan_by_lcid(w, scid);
            if (c && result == 0) c->our_cfg_ok = 1;
            break;
        }
        case 0x06: {                                        /* disconnection request */
            uint16_t dcid = RD16(d), scid = RD16(d + 2);
            chan_t *c = chan_by_lcid(w, dcid); if (c) c->used = 0;
            uint8_t r[4]; LE16(r, dcid); LE16(r + 2, scid);
            sig_send(w, 0x07, ident, r, 4);
            rt_log("wm", "remote %d: host closed L2CAP channel %04X", w->idx, dcid);
            break;
        }
        case 0x07: break;
        default: rt_log("wm", "unhandled L2CAP signal %02X", code); break;
        }
        p += 4 + clen; len -= 4 + clen;
    }
}

static wm_t *by_handle(uint16_t h) { for (int i = 0; i < NWM; i++) if (W[i].handle == h) return &W[i]; return NULL; }
static wm_t *by_addr(uint32_t a) {                          /* guest address of a BD_ADDR (LSB first) */
    for (int i = 0; i < NWM; i++) { int ok = 1; for (int k = 0; k < 6; k++) if (R8(a + k) != W[i].addr[k]) ok = 0; if (ok) return &W[i]; }
    return NULL;
}

void wiimote_acl_from_host(uint16_t handle, uint32_t data, uint16_t len) {
    uint8_t b[400];
    if (len > sizeof b) len = sizeof b;
    for (int i = 0; i < len; i++) b[i] = R8(data + i);
    /* flow control: tell the host the packet was consumed */
    uint8_t ev[7] = {0x13, 5, 1, handle & 0xFF, handle >> 8, 1, 0};
    in_update++;
    bt_send_event(ev, 7);
    wm_t *w = by_handle(handle);
    if (w && len >= 4) {
        uint16_t plen = RD16(b), cid = RD16(b + 2);
        const uint8_t *p = b + 4; if (plen > len - 4) plen = len - 4;
        if (cid == 0x0001) sig_recv(w, p, plen);
        else {
            chan_t *c = chan_by_lcid(w, cid);
            if (!c) rt_log("wm", "remote %d: data for unknown CID %04X", w->idx, cid);
            else if (c->psm == 0x11 || c->psm == 0x13) {
                uint8_t hdr = p[0];
                if (hdr == 0xA2 || hdr == 0x52) {
                    hid_output(w, p + 1, plen - 1);
                    if (hdr == 0x52) { uint8_t hs = 0x00; l2cap_send(w, c->rcid, &hs, 1); }   /* SET_REPORT handshake */
                }
            } else if (c->psm == 0x01) {
                /* SDP: answer with an error response; the Wii doesn't need SDP for a paired remote */
                uint8_t r[7] = {0x01, p[1], p[2], 0, 2, 0, 3};
                l2cap_send(w, c->rcid, r, 7);
            }
        }
    }
    in_update--;
    bt_update();
}

/* ---- HCI ---- */
static void init_all(void) {
    static int done; if (done) return; done = 1;
    eeprom_init();
    for (int i = 0; i < NWM; i++) {
        wm_t *w = &W[i]; memset(w, 0, sizeof *w);
        w->idx = i; w->handle = (uint16_t)(0x100 + i); w->dev = -1; w->sig_ident = 0x40; w->report_mode = 0x30;
        static const uint8_t a[6] = {0x11, 0x02, 0x19, 0x79, 0x00, 0x00};
        memcpy(w->addr, a, 6); w->addr[5] = (uint8_t)i;
        w->t_state = rt_emu_ns(); w->last_btn = 0xFFFFFFFF;
    }
}
int wiimote_link_keys_event(void) {
    init_all();
    uint8_t e[3 + 22 * NWM] = {0x15, 1 + 22 * NWM, NWM};
    for (int i = 0; i < NWM; i++) {
        memcpy(e + 3 + 22 * i, W[i].addr, 6);
        for (int k = 0; k < 16; k++) e[3 + 22 * i + 6 + k] = (uint8_t)(0xA0 + k + i);
    }
    bt_send_event(e, sizeof e);
    return NWM;
}
static void conn_complete(wm_t *w, uint8_t status) {
    uint8_t e[13] = {0x03, 11, status, w->handle & 0xFF, w->handle >> 8};
    memcpy(e + 5, w->addr, 6); e[11] = 1; e[12] = 0;
    bt_send_event(e, 13);
    if (status == 0) {
        w->state = ST_CONNECTED; w->t_state = rt_emu_ns();
        memset(w->ch, 0, sizeof w->ch); w->report_mode = 0x30; w->continuous = 0; w->rd.active = 0; w->last_btn = 0xFFFFFFFF; w->announced = 0;
        rt_log("wm", "remote %d connected", w->idx);
    }
}
static void link_down(wm_t *w, uint8_t reason) {           /* the remote goes away (or the host dropped it) */
    uint8_t e[6] = {0x05, 4, 0, w->handle & 0xFF, w->handle >> 8, reason};
    bt_send_event(e, 6);
    w->state = ST_IDLE; w->t_state = rt_emu_ns(); memset(w->ch, 0, sizeof w->ch); w->announced = 0;
}

int wiimote_hci_command(uint16_t op, uint32_t p) {
    init_all();
    switch (op) {
    case 0x0409: {                                          /* accept connection request */
        wm_t *w = by_addr(p); if (!w) return 0;
        uint8_t role = R8(p + 6);
        bt_cmd_status(op, 0);
        if (role == 0) { uint8_t e[10] = {0x12, 8, 0}; memcpy(e + 3, w->addr, 6); e[9] = 0; bt_send_event(e, 10); }
        conn_complete(w, 0);
        return 1;
    }
    case 0x040A: {                                          /* reject connection request */
        wm_t *w = by_addr(p); if (!w) return 0;
        bt_cmd_status(op, 0);
        uint8_t reason = R8(p + 6);
        uint8_t e[13] = {0x03, 11, reason ? reason : 0x0F, 0, 0}; memcpy(e + 5, w->addr, 6); e[11] = 1; e[12] = 0;
        bt_send_event(e, 13);
        rt_log("wm", "remote %d: the host rejected the connection (reason %02X)", w->idx, reason);
        w->state = ST_IDLE; w->t_state = rt_emu_ns();
        return 1;
    }
    case 0x0405: {                                          /* create connection (host pages us) */
        wm_t *w = by_addr(p); if (!w) return 0;
        bt_cmd_status(op, 0); conn_complete(w, 0); return 1;
    }
    case 0x0406: {                                          /* disconnect */
        wm_t *w = by_handle((uint16_t)((R8(p) | (R8(p + 1) << 8)) & 0x0FFF)); if (!w) return 0;
        bt_cmd_status(op, 0);
        rt_log("wm", "remote %d: the host disconnected it (reason %02X)", w->idx, R8(p + 2));
        link_down(w, 0x16);
        return 1;
    }
    case 0x040B: case 0x040C: {                             /* link key (negative) reply */
        uint8_t e[12] = {0x0E, 10, 1, op & 0xFF, op >> 8, 0}; for (int k = 0; k < 6; k++) e[6 + k] = R8(p + k);
        bt_send_event(e, 12); return 1;
    }
    case 0x040F: case 0x0411: case 0x041B: case 0x041D: case 0x041F: case 0x0803: {
        wm_t *w = by_handle((uint16_t)((R8(p) | (R8(p + 1) << 8)) & 0x0FFF)); if (!w) return 0;
        uint8_t hl = w->handle & 0xFF, hh = w->handle >> 8;
        bt_cmd_status(op, 0);
        if (op == 0x040F) { uint8_t e[7] = {0x1D, 5, 0, hl, hh, R8(p + 2), R8(p + 3)}; bt_send_event(e, 7); }         /* change packet type */
        else if (op == 0x0411) { uint8_t e[5] = {0x06, 3, 0, hl, hh}; bt_send_event(e, 5); }                        /* authentication */
        else if (op == 0x041B) { uint8_t e[13] = {0x0B, 11, 0, hl, hh, 0xBC, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00}; bt_send_event(e, 13); }   /* features */
        else if (op == 0x041D) { uint8_t e[10] = {0x0C, 8, 0, hl, hh, 0x02, 0x0F, 0x00, 0x29, 0x02}; bt_send_event(e, 10); }   /* version */
        else if (op == 0x041F) { uint8_t e[7] = {0x1C, 5, 0, hl, hh, 0x18, 0x38}; bt_send_event(e, 7); }             /* clock offset */
        else { uint8_t e[8] = {0x14, 6, 0, hl, hh, 2, R8(p + 2), R8(p + 3)}; bt_send_event(e, 8); }                 /* sniff mode */
        return 1;
    }
    case 0x0419: {                                          /* remote name request */
        wm_t *w = by_addr(p); if (!w) return 0;
        bt_cmd_status(op, 0);
        uint8_t e[2 + 255]; memset(e, 0, sizeof e);
        e[0] = 0x07; e[1] = 255; e[2] = 0; memcpy(e + 3, w->addr, 6); memcpy(e + 9, wm_name, sizeof wm_name);
        bt_send_event(e, sizeof e); return 1;
    }
    case 0x080D: {                                          /* write link policy settings */
        uint8_t e[8] = {0x0E, 6, 1, op & 0xFF, op >> 8, 0, R8(p), R8(p + 1)};
        bt_send_event(e, 8); return 1;
    }
    }
    return 0;
}

/* status for the menus: 0 no remote channel yet, 1 emulated (keyboard), 2 a real remote */
int wm_status(void) { return W[0].dev >= 0 ? 2 : chan_ready(chan_by_psm(&W[0], 0x13)) ? 1 : 0; }
/* is the game's remote `i` (WPAD channel) connected with its HID channels open? */
int wiimote_connected(int i) { return i >= 0 && i < NWM && W[i].state == ST_CONNECTED && chan_ready(chan_by_psm(&W[i], 0x13)); }

static void update_one(wm_t *w, uint64_t now) {
    int dev = remotes_slot_dev(w->idx);                     /* the real remote in this slot (-1 none) */
    int wanted = w->idx == 0 || dev >= 0 || remotes_slot_emulated(w->idx);   /* (remote 1 always: the title needs a player 1) */
    switch (w->state) {
    case ST_IDLE: {
        /* one at a time, lowest slot first, each after the one before is in the game (the
           module registers a player a few frames after its remote connects): the game then
           numbers its players in slot order when several are there at once */
        int busy = 0;
        for (int i = 0; i < w->idx; i++) {
            int want_i = i == 0 || remotes_slot_dev(i) >= 0 || remotes_slot_emulated(i);
            if (!want_i) continue;
            if (W[i].state == ST_REQUESTED) busy = 1;
            else if (W[i].state == ST_CONNECTED && (!W[i].announced || g_frames_presented - W[i].in_frame < 30)) busy = 1;
            else if (W[i].state == ST_IDLE && now - W[i].t_state < 5000000000ull) busy = 1;   /* (waiting for it, up to 5 s) */
        }
        if (wanted && !busy && (bt_scan_enable & 2) && now - w->t_state > 1000000000ull) {
            uint8_t e[12] = {0x04, 10}; memcpy(e + 2, w->addr, 6);
            e[8] = 0x00; e[9] = 0x04; e[10] = 0x48; e[11] = 1;   /* class of device, ACL link */
            rt_log("wm", "remote %d requests connection", w->idx);
            bt_send_event(e, 12);
            w->state = ST_REQUESTED; w->t_state = now;
        }
        break;
    }
    case ST_REQUESTED:
        if (now - w->t_state > 5000000000ull) { w->state = ST_IDLE; w->t_state = now; }   /* retry */
        break;
    case ST_CONNECTED: {
        if (!wanted) {                                      /* its real remote went away */
            rt_log("wm", "remote %d disconnects", w->idx);
            if (w->dev >= 0) w->dev = -1;
            link_down(w, 0x13);
            break;
        }
        if (now - w->t_state < 200000000ull) break;        /* let the host finish its HCI setup */
        chan_t *cc = chan_by_psm(w, 0x11), *ci = chan_by_psm(w, 0x13);
        if (!cc) { cc = chan_new(w, 0x11); send_conn_req(w, cc); break; }
        if (!chan_ready(cc)) break;
        if (!ci) { ci = chan_new(w, 0x13); send_conn_req(w, ci); break; }
        if (!chan_ready(ci)) break;
        if (!w->announced) { w->announced = 1; w->in_frame = g_frames_presented; rt_log("wm", "remote %d: HID channels open", w->idx); }
        if (dev != w->dev) {                                /* real remote attached / changed / gone */
            if (dev >= 0) real_attach(w, dev);
            else { w->dev = -1; rt_log("wm", "remote %d: real Wii Remote gone", w->idx); send_status(w); }
        }
        if (w->dev >= 0) { real_pump(w); break; }
        w->game_btn = remotes_game_buttons(w->idx, 0);           /* the keyboard, if it's this player's */
        if (bt_acl_queued() > 2) break;                    /* don't flood the host */
        if (w->rd.active) { read_step(w); break; }
        uint32_t btn = w->game_btn;
        int periodic = w->continuous || w->report_mode != 0x30;
        if (btn != w->last_btn || (periodic && now - w->t_last_report >= 10000000ull)) {
            w->t_last_report = now;
            send_data_report(w);
        }
        break;
    }
    }
}

void wiimote_update(void) {
    init_all();
    if (in_update) return;
    in_update++;
    remotes_update();                                       /* rt_remotes.c: real remotes come and go */
    uint64_t now = rt_emu_ns();
    for (int i = 0; i < NWM; i++) update_one(&W[i], now);
    in_update--;
}
