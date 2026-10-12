/* rt_hdtex.h - custom (HD) texture packs, Dolphin naming */
#pragma once
#include <stdint.h>
#include <stddef.h>
enum { HD_IDLE, HD_QUEUED, HD_READY, HD_FAILED, HD_UPLOADED };
enum { HD_RGBA8 = 1, HD_BGRA8, HD_BC1, HD_BC2, HD_BC3, HD_BC7 };
typedef struct hdtex {
    char *name, *path; int dds;
    volatile int state;
    int w, h, fmt, nlev;
    struct { size_t off, size; int w, h; } lev[16];
    uint8_t *data;                 /* loaded file (DDS) or decoded pixels (PNG), freed after upload */
    uint32_t gltex;
    struct hdtex *next;
} hdtex;
extern volatile uint32_t g_hd_ready;
void hd_init(void);
int hd_enabled(void);
hdtex *hd_find(const char *name);
void hd_request(hdtex *e);
hdtex *hd_lookup(const uint8_t *data, size_t size, const uint8_t *tlut, size_t tlut_avail, uint32_t w, uint32_t h, uint32_t fmt, int mipmapped);
