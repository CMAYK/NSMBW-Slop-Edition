/* rt_bt.c - emulated Bluetooth host controller behind IOS /dev/usb/oh1/57e/305.
 * USB V0 ioctlv: CTRLMSG = HCI command, INTRMSG ep 0x81 = HCI event buffer to fill,
 * BLKMSG ep 0x02 = ACL out (to remotes), ep 0x82 = ACL in buffer to fill.
 * Command replies follow Dolphin's BTEmu. Wii Remote (L2CAP/HID) lives in rt_wiimote.c. */
#include "rt.h"
#include "rt_bt.h"
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

void ios_reply(uint32_t req, int32_t result);

static uint32_t hci_ep_req, hci_ep_buf;          /* pending event buffer request */
static uint32_t acl_ep_req, acl_ep_buf;          /* pending ACL-in buffer request */
typedef struct { uint8_t data[300]; int len; } pkt;
static pkt evq[64]; static int nev;
static pkt aclq[64]; static int nacl;
int bt_scan_enable;
int bt_acl_queued(void) { return nacl; }

static void push(pkt *q, int *n, const uint8_t *d, int len) {
    if (*n >= 64) { rt_log("bt", "queue overflow"); return; }
    memcpy(q[*n].data, d, len); q[*n].len = len; (*n)++;
}
static void pop(pkt *q, int *n) { memmove(q, q + 1, sizeof *q * --(*n)); }

void bt_send_event(const uint8_t *ev, int len) { push(evq, &nev, ev, len); bt_update(); }
void bt_send_acl(uint16_t handle, const uint8_t *data, int len) {
    uint8_t b[300];
    b[0] = handle & 0xFF; b[1] = ((handle >> 8) & 0x0F) | 0x20;   /* packet start, point-to-point */
    b[2] = len & 0xFF; b[3] = len >> 8;
    memcpy(b + 4, data, len);
    push(aclq, &nacl, b, len + 4); bt_update();
}

void bt_update(void) {
    if (nev && hci_ep_req) {
        memcpy(G(hci_ep_buf), evq[0].data, evq[0].len);
        uint32_t r = hci_ep_req; hci_ep_req = 0;
        ios_reply(r, evq[0].len);
        pop(evq, &nev);
    }
    if (nacl && acl_ep_req && !nev) {
        memcpy(G(acl_ep_buf), aclq[0].data, aclq[0].len);
        uint32_t r = acl_ep_req; acl_ep_req = 0;
        ios_reply(r, aclq[0].len);
        pop(aclq, &nacl);
    }
    wiimote_update();
}

static void cmd_complete(uint16_t op, const uint8_t *ret, int n) {
    uint8_t e[260]; e[0] = 0x0E; e[1] = (uint8_t)(3 + n); e[2] = 1; e[3] = op & 0xFF; e[4] = op >> 8;
    memcpy(e + 5, ret, n); bt_send_event(e, 5 + n);
}
void bt_cmd_status(uint16_t op, uint8_t status) {
    uint8_t e[6] = {0x0F, 4, status, 1, op & 0xFF, op >> 8}; bt_send_event(e, 6);
}

const uint8_t bt_host_addr[6] = {0x11, 0x02, 0x19, 0x79, 0x00, 0xFF};

static void hci_command(uint32_t pkt_addr) {
    uint16_t op = R8(pkt_addr) | (R8(pkt_addr + 1) << 8);
    uint32_t p = pkt_addr + 3;
    uint8_t st0 = 0;
    LOG("bt", "HCI command %04X", op);
    switch (op) {
    case 0x1005: { uint8_t r[] = {0, 0x53, 0x01, 0x40, 10, 0, 0, 0}; cmd_complete(op, r, sizeof r); return; }
    case 0x1001: { uint8_t r[] = {0, 3, 0xA7, 0x40, 3, 0x0F, 0x00, 0x0E, 0x43}; cmd_complete(op, r, sizeof r); return; }
    case 0x1009: { uint8_t r[7] = {0}; memcpy(r + 1, bt_host_addr, 6); cmd_complete(op, r, 7); return; }
    case 0x1003: { uint8_t r[] = {0, 0xFF, 0xFF, 0x8D, 0xFE, 0x9B, 0xF9, 0x00, 0x80}; cmd_complete(op, r, sizeof r); return; }
    case 0x0C0D: {                                          /* Read stored link keys */
        int n = wiimote_link_keys_event();
        uint8_t r[] = {0, 0xFF, 0, (uint8_t)n, 0}; cmd_complete(op, r, sizeof r); return;
    }
    case 0x0C12: { uint8_t r[] = {0, 1, 0}; cmd_complete(op, r, sizeof r); return; }   /* delete stored key */
    case 0x0C1A: bt_scan_enable = R8(p); cmd_complete(op, &st0, 1); return;
    case 0x0C37: { uint8_t r[] = {0, R8(p), R8(p + 1)}; cmd_complete(op, r, 3); return; }
    case 0x0401: {                                          /* Inquiry: nothing out there */
        bt_cmd_status(op, 0); uint8_t e[] = {0x01, 1, 0}; bt_send_event(e, 3); return;
    }
    case 0x0402: cmd_complete(op, &st0, 1); return;
    default:
        if (wiimote_hci_command(op, p)) return;             /* connection-related commands */
        cmd_complete(op, &st0, 1);
    }
}

int32_t bt_ioctlv(uint32_t req, uint32_t ioc, uint32_t nin, uint32_t nout, uint32_t vec, int *pending) {
    (void)nout;
    switch (ioc) {
    case 0: {                                               /* control message: HCI command */
        uint32_t payload = R32(vec + 8 * nin);
        uint32_t wlen = R8(R32(vec + 8 * 4)) | (R8(R32(vec + 8 * 4) + 1) << 8);   /* wLength, little-endian */
        hci_command(payload);
        return (int32_t)wlen;
    }
    case 1: {                                               /* bulk */
        uint8_t ep = R8(R32(vec));
        uint32_t buf = R32(vec + 8 * nin);
        if (ep == 0x02) {                                   /* ACL out: host -> remote */
            uint16_t h = (R8(buf) | (R8(buf + 1) << 8)) & 0x0FFF, len = R8(buf + 2) | (R8(buf + 3) << 8);
            wiimote_acl_from_host(h, buf + 4, len);
            return 0;
        }
        if (ep == 0x82) { acl_ep_req = req; acl_ep_buf = buf; *pending = 1; bt_update(); return 0; }
        return 0;
    }
    case 2: {                                               /* interrupt: HCI event buffer */
        uint8_t ep = R8(R32(vec));
        if (ep == 0x81) { hci_ep_req = req; hci_ep_buf = R32(vec + 8 * nin); *pending = 1; bt_update(); return 0; }
        return 0;
    }
    }
    rt_log("bt", "unknown oh1 ioctlv %u", ioc);
    return 0;
}
