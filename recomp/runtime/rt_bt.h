#pragma once
#include <stdint.h>
int32_t bt_ioctlv(uint32_t req, uint32_t ioc, uint32_t nin, uint32_t nout, uint32_t vec, int *pending);
void bt_update(void);
void bt_send_event(const uint8_t *ev, int len);
void bt_send_acl(uint16_t handle, const uint8_t *data, int len);
void bt_cmd_status(uint16_t op, uint8_t status);
extern const uint8_t bt_host_addr[6];
extern int bt_scan_enable;
int bt_acl_queued(void);
/* rt_wiimote.c */
void wiimote_update(void);
int  wiimote_link_keys_event(void);
int  wiimote_hci_command(uint16_t op, uint32_t params);
void wiimote_acl_from_host(uint16_t handle, uint32_t data, uint16_t len);
