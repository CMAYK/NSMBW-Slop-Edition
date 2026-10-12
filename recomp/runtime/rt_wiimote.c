/* rt_wiimote.c - one emulated Wii Remote (no extension) on the emulated Bluetooth controller.
 * Mirrors Dolphin's emulated remote: the remote is registered in SYSCONF (BT.DINF), pages the
 * host once page scan is enabled, then opens the HID control (PSM 0x11) and interrupt
 * (PSM 0x13) L2CAP channels itself and speaks the Wii Remote HID protocol on them.
 * Buttons come from plat_buttons() (keyboard) and a real remote, filtered by the in-game menus (rt_menu.c). */
#include "rt.h"
#include "rt_bt.h"

#define HANDLE 0x100
static const uint8_t wm_addr[6] = {0x11, 0x02, 0x19, 0x79, 0x00, 0x00};   /* HCI (LSB-first) order */
static const char wm_name[] = "Nintendo RVL-CNT-01";

enum { ST_IDLE, ST_REQUESTED, ST_CONNECTED };
static int state = ST_IDLE;
static uint64_t t_state;          /* ns of last state change */
static int in_update;

/* ---- L2CAP ---- */
typedef struct {
    int used;
    uint16_t psm, lcid, rcid;
    int our_cfg_sent, our_cfg_ok, their_cfg_ok;
} chan_t;
static chan_t ch[4];
static uint8_t sig_ident = 0x40;

static chan_t *chan_by_lcid(uint16_t c) { for (int i = 0; i < 4; i++) if (ch[i].used && ch[i].lcid == c) return &ch[i]; return NULL; }
static chan_t *chan_by_psm(uint16_t p) { for (int i = 0; i < 4; i++) if (ch[i].used && ch[i].psm == p) return &ch[i]; return NULL; }
static chan_t *chan_new(uint16_t psm) {
    for (int i = 0; i < 4; i++) if (!ch[i].used) {
        memset(&ch[i], 0, sizeof ch[i]); ch[i].used = 1; ch[i].psm = psm; ch[i].lcid = 0x40 + i; return &ch[i];
    }
    return NULL;
}
static int chan_ready(chan_t *c) { return c && c->rcid && c->our_cfg_ok && c->their_cfg_ok; }

static void l2cap_send(uint16_t cid, const uint8_t *d, int len) {
    uint8_t b[300];
    b[0] = len & 0xFF; b[1] = len >> 8; b[2] = cid & 0xFF; b[3] = cid >> 8;
    memcpy(b + 4, d, len);
    bt_send_acl(HANDLE, b, len + 4);
}
static void sig_send(uint8_t code, uint8_t ident, const uint8_t *d, int len) {
    uint8_t b[64]; b[0] = code; b[1] = ident; b[2] = len & 0xFF; b[3] = len >> 8;
    memcpy(b + 4, d, len);
    l2cap_send(0x0001, b, len + 4);
}
#define LE16(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((v) >> 8); } while (0)
#define RD16(p) ((uint16_t)((p)[0] | ((p)[1] << 8)))

static void send_conn_req(chan_t *c) {
    uint8_t d[4]; LE16(d, c->psm); LE16(d + 2, c->lcid);
    LOG("wm", "L2CAP connect request psm %02X", c->psm);
    sig_send(0x02, sig_ident++, d, 4);
}
static void send_cfg_req(chan_t *c) {
    uint8_t d[8]; LE16(d, c->rcid); LE16(d + 2, 0); d[4] = 0x01; d[5] = 2; LE16(d + 6, 185);   /* MTU 185 */
    c->our_cfg_sent = 1;
    sig_send(0x04, sig_ident++, d, 8);
}

/* ---- remote state ---- */
static uint8_t leds, report_mode = 0x30, continuous, ir_on, speaker_on;
static uint32_t last_btn = 0xFFFFFFFF;
static uint64_t t_last_report;
static int shake_phase;
static uint8_t eeprom[0x1700];
static struct { int active; uint32_t addr; int left; int space; } rd;   /* pending memory read */

static void eeprom_init(void) {
    static const uint8_t cal[10] = {0x80, 0x80, 0x80, 0x00, 0x9A, 0x9A, 0x9A, 0x00, 0x40, 0xE3};
    /* IR calibration (Dolphin defaults) at 0x00 and 0x0B */
    static const uint8_t ir[11] = {0x66, 0x6E, 0x77, 0xB8, 0x83, 0x71, 0x89, 0x88, 0x30, 0x00, 0x00};
    memcpy(eeprom + 0x00, ir, 11); memcpy(eeprom + 0x0B, ir, 11);
    memcpy(eeprom + 0x16, cal, 10); memcpy(eeprom + 0x20, cal, 10);
}

static uint32_t game_btn;          /* buttons the game sees (keyboard + real remote, after the menus) */
static void hid_send(const uint8_t *d, int len) {           /* input report on interrupt channel */
    chan_t *c = chan_by_psm(0x13);
    if (!chan_ready(c)) return;
    uint8_t b[32]; b[0] = 0xA1; memcpy(b + 1, d, len);
    l2cap_send(c->rcid, b, len + 1);
}
static void put_buttons(uint8_t *p, uint32_t btn) { p[0] = btn & 0x1F; p[1] = (btn >> 8) & 0x9F; }

static void send_ack(uint8_t rpt, uint8_t err) {
    uint8_t r[5] = {0x22}; put_buttons(r + 1, game_btn); r[3] = rpt; r[4] = err; hid_send(r, 5);
}
static void send_status(void) {
    uint8_t r[7] = {0x20};
    put_buttons(r + 1, game_btn);
    r[3] = (speaker_on ? 0x04 : 0) | (ir_on ? 0x08 : 0) | (leds << 4);
    r[6] = 0xC0;                                            /* battery */
    hid_send(r, 7);
}

/* shake (keyboard): a strong back-and-forth swing that flips every second report, so the game
   sees the motion change from one frame to the next whatever the report timing. A tap shakes
   for at least 16 reports (~160 ms) so short presses register. Returns 1 while shaking (and
   sets the accelerometer bytes). Called once per report sent. */
static int kb_shake_accel(uint32_t btn, uint8_t *ax, uint8_t *ay, uint8_t *az) {
    static int left, n;
    if (btn & 0x10000) { if (!shake_phase) { shake_phase = 1; n = 0; } left = 16; }
    else if (shake_phase && --left <= 0) shake_phase = 0;
    if (!shake_phase) return 0;
    int ph = (n++ >> 1) & 1;
    *ax = ph ? 0x10 : 0xF0; *ay = ph ? 0xF0 : 0x10; *az = ph ? 0x30 : 0xE0;
    return 1;
}
static void send_data_report(void) {
    uint32_t btn = game_btn;
    uint8_t r[22]; memset(r, 0, sizeof r);
    r[0] = report_mode; put_buttons(r + 1, btn);
    uint8_t ax = 0x80, ay = 0x80, az = 0x9A;
    kb_shake_accel(btn, &ax, &ay, &az);
    int n;
    switch (report_mode) {
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
    hid_send(r, n);
    last_btn = btn;
}

static void read_step(void) {                               /* one 0x21 report of a pending read */
    uint8_t r[22]; memset(r, 0, sizeof r);
    r[0] = 0x21; put_buttons(r + 1, game_btn);
    int n = rd.left > 16 ? 16 : rd.left, err = 0;
    if (rd.space == 0) {                                    /* EEPROM */
        if (rd.addr + n > sizeof eeprom) err = 8;
        else memcpy(r + 6, eeprom + rd.addr, n);
    } else {                                                /* control registers */
        uint32_t reg = (rd.addr >> 16) & 0xFE;
        if (reg == 0xA4) err = 7;                           /* no extension connected */
    }
    r[3] = (uint8_t)(((n - 1) << 4) | err);
    r[4] = (rd.addr >> 8) & 0xFF; r[5] = rd.addr & 0xFF;
    hid_send(r, 22);
    if (err) { rd.active = 0; return; }
    rd.addr += n; rd.left -= n;
    if (rd.left <= 0) rd.active = 0;
}

/* ---- real Wii Remote passthrough (plat_wiimote_*: paired over Bluetooth on the PC) ----
 * The game's output reports go to the real remote and its input reports come back unchanged;
 * keyboard buttons are merged into them. Reports that set the remote's state are remembered and
 * replayed when a remote (re)connects. Without a real remote the emulated one above is used. */
static int real;
static uint8_t saved_out[0x20][23]; static int saved_len[0x20];
static uint8_t pend[64][23]; static int pend_len[64], npend;
static int is_data_report(uint8_t id) { return id >= 0x30 && id <= 0x3F; }
static uint32_t real_btn;           /* latest buttons reported by the real remote */
static int has_buttons(const uint8_t *r, int n) { return n >= 3 && ((r[0] >= 0x20 && r[0] <= 0x22) || (r[0] >= 0x30 && r[0] <= 0x37)); }
static void merge_keyboard(uint8_t *r, int n) {     /* real buttons + keyboard, filtered by the menus */
    if (!has_buttons(r, n)) return;
    r[1] = (r[1] & ~0x1F) | (game_btn & 0x1F); r[2] = (r[2] & ~0x9F) | ((game_btn >> 8) & 0x9F);
    uint8_t ax, ay, az;                              /* keyboard shake overrides the remote's motion */
    if ((r[0] == 0x31 || r[0] == 0x33 || r[0] == 0x35 || r[0] == 0x37) && n >= 6 && kb_shake_accel(game_btn, &ax, &ay, &az)) { r[3] = ax; r[4] = ay; r[5] = az; }
}
static void real_pump(void) {
    uint8_t r[23]; int n;
    while ((n = plat_wiimote_read(r, sizeof r)) > 0) {
        if (npend && is_data_report(r[0]) && is_data_report(pend[npend - 1][0])) npend--;   /* keep only the newest data report */
        if (npend == 64) { memmove(pend, pend + 1, sizeof pend[0] * 63); memmove(pend_len, pend_len + 1, sizeof pend_len[0] * 63); npend--; }
        memcpy(pend[npend], r, n); pend_len[npend++] = n;
        if (has_buttons(r, n)) real_btn = (r[1] & 0x1F) | ((uint32_t)(r[2] & 0x9F) << 8);
    }
    game_btn = menu_filter_buttons(plat_buttons() | real_btn);
    while (npend && bt_acl_queued() <= 2) {
        merge_keyboard(pend[0], pend_len[0]);
        hid_send(pend[0], pend_len[0]);
        memmove(pend, pend + 1, sizeof pend[0] * (npend - 1)); memmove(pend_len, pend_len + 1, sizeof pend_len[0] * (npend - 1)); npend--;
    }
}
int wm_status(void) { return real ? 2 : chan_ready(chan_by_psm(0x13)) ? 1 : 0; }
static void real_attach(void) {
    uint8_t r[23]; while (plat_wiimote_read(r, sizeof r) > 0) {}      /* drop what arrived before */
    npend = 0;
    static const uint8_t order[] = {0x11, 0x13, 0x1A, 0x14, 0x19, 0x12};   /* LEDs, IR, speaker, then reporting mode */
    for (size_t i = 0; i < sizeof order; i++) if (saved_len[order[i]]) plat_wiimote_write(saved_out[order[i]], saved_len[order[i]]);
    uint8_t st[2] = {0x15, 0}; plat_wiimote_write(st, 2);             /* status report: tells the game about extensions/battery */
    rt_log("wm", "using the real Wii Remote");
}

static void hid_output(const uint8_t *d, int len) {         /* d[0] = report id */
    if (len < 2) return;
    uint8_t id = d[0];
    int ack = d[1] & 0x02;
    if (real) {
        switch (id) {                                       /* keep our view of the state for keyboard fallback */
        case 0x11: leds = d[1] >> 4; break;
        case 0x12: continuous = (d[1] & 0x04) != 0; report_mode = len > 2 ? d[2] : 0x30; break;
        case 0x13: case 0x1A: ir_on = (d[1] & 0x04) != 0; break;
        case 0x14: speaker_on = (d[1] & 0x04) != 0; break;
        }
        if (id == 0x11 || id == 0x12 || id == 0x13 || id == 0x14 || id == 0x19 || id == 0x1A) { memcpy(saved_out[id], d, len > 23 ? 23 : len); saved_len[id] = len > 23 ? 23 : len; }
        plat_wiimote_write(d, len);
        return;
    }
    if (id == 0x11 || id == 0x12 || id == 0x13 || id == 0x14 || id == 0x19 || id == 0x1A) { memcpy(saved_out[id], d, len > 23 ? 23 : len); saved_len[id] = len > 23 ? 23 : len; }
    LOG("wm", "HID out %02X %02X len %d", id, d[1], len);
    switch (id) {
    case 0x10: break;                                       /* rumble */
    case 0x11: leds = d[1] >> 4; if (ack) send_ack(id, 0); break;
    case 0x12:
        continuous = (d[1] & 0x04) != 0; report_mode = d[2];
        rt_log("wm", "reporting mode %02X%s", report_mode, continuous ? " (continuous)" : "");
        if (ack) send_ack(id, 0);
        send_data_report();
        break;
    case 0x13: case 0x1A: ir_on = (d[1] & 0x04) != 0; if (ack) send_ack(id, 0); break;
    case 0x14: speaker_on = (d[1] & 0x04) != 0; if (ack) send_ack(id, 0); break;
    case 0x19: if (ack) send_ack(id, 0); break;
    case 0x15: send_status(); break;
    case 0x16: {                                            /* write memory / registers */
        uint32_t addr = (d[2] << 16) | (d[3] << 8) | d[4]; int n = d[5];
        if (!(d[1] & 0x04) && addr + n <= sizeof eeprom && len >= 6 + n) memcpy(eeprom + addr, d + 6, n);
        send_ack(id, 0);
        break;
    }
    case 0x17: {                                            /* read memory / registers */
        rd.space = (d[1] & 0x04) ? 1 : 0;
        rd.addr = (d[2] << 16) | (d[3] << 8) | d[4];
        rd.left = (d[5] << 8) | d[6];
        rd.active = rd.left > 0;
        LOG("wm", "read %s %06X size %d", rd.space ? "reg" : "eeprom", rd.addr, rd.left);
        if (rd.space == 0) rd.addr &= 0xFFFF;
        break;
    }
    case 0x18: break;                                       /* speaker data */
    default: rt_log("wm", "unhandled HID output report %02X", id); break;
    }
}

/* ---- L2CAP signalling from the host ---- */
static void sig_recv(const uint8_t *p, int len) {
    while (len >= 4) {
        uint8_t code = p[0], ident = p[1]; uint16_t clen = RD16(p + 2);
        const uint8_t *d = p + 4;
        switch (code) {
        case 0x02: {                                        /* connection request (host -> us) */
            uint16_t psm = RD16(d), scid = RD16(d + 2);
            chan_t *c = chan_by_psm(psm); if (!c) c = chan_new(psm);
            uint8_t r[8];
            LOG("wm", "host connect psm %02X scid %04X", psm, scid);
            if (!c) { LE16(r, 0); LE16(r + 2, scid); LE16(r + 4, 4); LE16(r + 6, 0); sig_send(0x03, ident, r, 8); break; }
            c->rcid = scid;
            LE16(r, c->lcid); LE16(r + 2, scid); LE16(r + 4, 0); LE16(r + 6, 0);
            sig_send(0x03, ident, r, 8);
            send_cfg_req(c);
            break;
        }
        case 0x03: {                                        /* connection response */
            uint16_t dcid = RD16(d), scid = RD16(d + 2), result = RD16(d + 4);
            chan_t *c = chan_by_lcid(scid);
            LOG("wm", "connect response dcid %04X scid %04X result %d", dcid, scid, result);
            if (c && result == 0) { c->rcid = dcid; if (!c->our_cfg_sent) send_cfg_req(c); }
            else if (c && result != 1) c->used = 0;
            break;
        }
        case 0x04: {                                        /* configuration request: accept as-is */
            uint16_t dcid = RD16(d);
            chan_t *c = chan_by_lcid(dcid);
            uint8_t r[64]; int olen = clen - 4; if (olen < 0) olen = 0; if (olen > 50) olen = 50;
            LE16(r, c ? c->rcid : 0); LE16(r + 2, 0); LE16(r + 4, 0);
            memcpy(r + 6, d + 4, olen);
            sig_send(0x05, ident, r, 6 + olen);
            if (c) { c->their_cfg_ok = 1; if (!c->our_cfg_sent && c->rcid) send_cfg_req(c); }
            LOG("wm", "config request for %04X", dcid);
            break;
        }
        case 0x05: {                                        /* configuration response */
            uint16_t scid = RD16(d), result = RD16(d + 4);
            chan_t *c = chan_by_lcid(scid);
            if (c && result == 0) c->our_cfg_ok = 1;
            LOG("wm", "config response for %04X result %d", scid, result);
            break;
        }
        case 0x06: {                                        /* disconnection request */
            uint16_t dcid = RD16(d), scid = RD16(d + 2);
            chan_t *c = chan_by_lcid(dcid); if (c) c->used = 0;
            uint8_t r[4]; LE16(r, dcid); LE16(r + 2, scid);
            sig_send(0x07, ident, r, 4);
            rt_log("wm", "host closed L2CAP channel %04X", dcid);
            break;
        }
        case 0x07: break;
        default: rt_log("wm", "unhandled L2CAP signal %02X", code); break;
        }
        p += 4 + clen; len -= 4 + clen;
    }
}

void wiimote_acl_from_host(uint16_t handle, uint32_t data, uint16_t len) {
    uint8_t b[400];
    if (len > sizeof b) len = sizeof b;
    for (int i = 0; i < len; i++) b[i] = R8(data + i);
    /* flow control: tell the host the packet was consumed */
    uint8_t ev[7] = {0x13, 5, 1, handle & 0xFF, handle >> 8, 1, 0};
    in_update++;
    bt_send_event(ev, 7);
    if (handle == HANDLE && len >= 4) {
        uint16_t plen = RD16(b), cid = RD16(b + 2);
        const uint8_t *p = b + 4; if (plen > len - 4) plen = len - 4;
        if (cid == 0x0001) sig_recv(p, plen);
        else {
            chan_t *c = chan_by_lcid(cid);
            if (!c) rt_log("wm", "data for unknown CID %04X", cid);
            else if (c->psm == 0x11 || c->psm == 0x13) {
                uint8_t hdr = p[0];
                if (hdr == 0xA2 || hdr == 0x52) {
                    hid_output(p + 1, plen - 1);
                    if (hdr == 0x52) { uint8_t hs = 0x00; l2cap_send(c->rcid, &hs, 1); }   /* SET_REPORT handshake */
                } else LOG("wm", "HID transaction %02X", hdr);
            } else if (c->psm == 0x01) {
                /* SDP: answer with an error response; the Wii doesn't need SDP for a paired remote */
                uint8_t r[7] = {0x01, p[1], p[2], 0, 2, 0, 3};
                l2cap_send(c->rcid, r, 7);
            }
        }
    }
    in_update--;
    bt_update();
}

/* ---- HCI ---- */
int wiimote_link_keys_event(void) {
    uint8_t e[2 + 1 + 22] = {0x15, 23, 1};
    memcpy(e + 3, wm_addr, 6);
    for (int i = 0; i < 16; i++) e[9 + i] = (uint8_t)(0xA0 + i);
    bt_send_event(e, sizeof e);
    return 1;
}

static void conn_complete(uint8_t status) {
    uint8_t e[13] = {0x03, 11, status, HANDLE & 0xFF, HANDLE >> 8};
    memcpy(e + 5, wm_addr, 6); e[11] = 1; e[12] = 0;
    bt_send_event(e, 13);
    if (status == 0) {
        state = ST_CONNECTED; t_state = rt_now_ns();
        memset(ch, 0, sizeof ch); report_mode = 0x30; continuous = 0; rd.active = 0; last_btn = 0xFFFFFFFF;
        rt_log("wm", "Wii Remote connected");
    }
}

int wiimote_hci_command(uint16_t op, uint32_t p) {
    switch (op) {
    case 0x0409: {                                          /* accept connection request */
        uint8_t role = R8(p + 6);
        bt_cmd_status(op, 0);
        if (role == 0) { uint8_t e[10] = {0x12, 8, 0}; memcpy(e + 3, wm_addr, 6); e[9] = 0; bt_send_event(e, 10); }
        conn_complete(0);
        return 1;
    }
    case 0x040A: {                                          /* reject connection request */
        bt_cmd_status(op, 0);
        uint8_t reason = R8(p + 6);
        uint8_t e[13] = {0x03, 11, reason ? reason : 0x0F, 0, 0}; memcpy(e + 5, wm_addr, 6); e[11] = 1; e[12] = 0;
        bt_send_event(e, 13);
        rt_log("wm", "host rejected the Wii Remote connection (reason %02X)", reason);
        state = ST_IDLE; t_state = rt_now_ns();
        return 1;
    }
    case 0x0405:                                            /* create connection (host pages us) */
        bt_cmd_status(op, 0); conn_complete(0); return 1;
    case 0x0406: {                                          /* disconnect */
        bt_cmd_status(op, 0);
        uint8_t e[6] = {0x05, 4, 0, HANDLE & 0xFF, HANDLE >> 8, 0x16};
        bt_send_event(e, 6);
        rt_log("wm", "host disconnected the Wii Remote (reason %02X)", R8(p + 2));
        state = ST_IDLE; t_state = rt_now_ns(); memset(ch, 0, sizeof ch);
        return 1;
    }
    case 0x040B: case 0x040C: {                             /* link key (negative) reply */
        uint8_t e[12] = {0x0E, 10, 1, op & 0xFF, op >> 8, 0}; memcpy(e + 6, wm_addr, 6);
        bt_send_event(e, 12); return 1;
    }
    case 0x040F: {                                          /* change connection packet type */
        bt_cmd_status(op, 0);
        uint8_t e[7] = {0x1D, 5, 0, HANDLE & 0xFF, HANDLE >> 8, R8(p + 2), R8(p + 3)};
        bt_send_event(e, 7); return 1;
    }
    case 0x0411: {                                          /* authentication requested */
        bt_cmd_status(op, 0);
        uint8_t e[5] = {0x06, 3, 0, HANDLE & 0xFF, HANDLE >> 8};
        bt_send_event(e, 5); return 1;
    }
    case 0x0419: {                                          /* remote name request */
        bt_cmd_status(op, 0);
        uint8_t e[2 + 255]; memset(e, 0, sizeof e);
        e[0] = 0x07; e[1] = 255; e[2] = 0; memcpy(e + 3, wm_addr, 6); memcpy(e + 9, wm_name, sizeof wm_name);
        bt_send_event(e, sizeof e); return 1;
    }
    case 0x041B: {                                          /* read remote features */
        bt_cmd_status(op, 0);
        uint8_t e[13] = {0x0B, 11, 0, HANDLE & 0xFF, HANDLE >> 8, 0xBC, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00};
        bt_send_event(e, 13); return 1;
    }
    case 0x041D: {                                          /* read remote version */
        bt_cmd_status(op, 0);
        uint8_t e[10] = {0x0C, 8, 0, HANDLE & 0xFF, HANDLE >> 8, 0x02, 0x0F, 0x00, 0x29, 0x02};
        bt_send_event(e, 10); return 1;
    }
    case 0x041F: {                                          /* read clock offset */
        bt_cmd_status(op, 0);
        uint8_t e[7] = {0x1C, 5, 0, HANDLE & 0xFF, HANDLE >> 8, 0x18, 0x38};
        bt_send_event(e, 7); return 1;
    }
    case 0x0803: {                                          /* sniff mode */
        bt_cmd_status(op, 0);
        uint8_t e[8] = {0x14, 6, 0, HANDLE & 0xFF, HANDLE >> 8, 2, R8(p + 2), R8(p + 3)};
        bt_send_event(e, 8); return 1;
    }
    case 0x080D: {                                          /* write link policy settings */
        uint8_t e[8] = {0x0E, 6, 1, op & 0xFF, op >> 8, 0, R8(p), R8(p + 1)};
        bt_send_event(e, 8); return 1;
    }
    }
    return 0;
}

void wiimote_update(void) {
    static int inited;
    if (!inited) { inited = 1; eeprom_init(); t_state = rt_now_ns(); }
    if (in_update) return;
    in_update++;
    uint64_t now = rt_now_ns();
    switch (state) {
    case ST_IDLE:
        if ((bt_scan_enable & 2) && now - t_state > 1000000000ull) {
            uint8_t e[12] = {0x04, 10}; memcpy(e + 2, wm_addr, 6);
            e[8] = 0x00; e[9] = 0x04; e[10] = 0x48; e[11] = 1;   /* class of device, ACL link */
            rt_log("wm", "Wii Remote requests connection");
            bt_send_event(e, 12);
            state = ST_REQUESTED; t_state = now;
        }
        break;
    case ST_REQUESTED:
        if (now - t_state > 5000000000ull) { state = ST_IDLE; t_state = now; }   /* retry */
        break;
    case ST_CONNECTED: {
        if (now - t_state < 200000000ull) break;           /* let the host finish its HCI setup */
        chan_t *cc = chan_by_psm(0x11), *ci = chan_by_psm(0x13);
        if (!cc) { cc = chan_new(0x11); send_conn_req(cc); break; }
        if (!chan_ready(cc)) break;
        if (!ci) { ci = chan_new(0x13); send_conn_req(ci); break; }
        if (!chan_ready(ci)) break;
        static int announced;
        if (!announced) { announced = 1; rt_log("wm", "HID channels open"); }
        {   /* real remote attached / detached? */
            int pr = plat_wiimote_present();
            if (pr && !real) { real = 1; rd.active = 0; real_attach(); }
            else if (!pr && real) { real = 0; rt_log("wm", "real Wii Remote gone: keyboard controls"); send_status(); }
            if (real) { real_pump(); break; }
            game_btn = menu_filter_buttons(plat_buttons());
        }
        if (bt_acl_queued() > 2) break;                    /* don't flood the host */
        if (rd.active) { read_step(); break; }
        uint32_t btn = game_btn;
        int periodic = continuous || report_mode != 0x30;
        if (btn != last_btn || (periodic && now - t_last_report >= 10000000ull)) {
            t_last_report = now;
            send_data_report();
        }
        break;
    }
    }
    in_update--;
}
