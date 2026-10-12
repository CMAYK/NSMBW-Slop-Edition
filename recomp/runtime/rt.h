/* rt.h - shared declarations for the NSMBW native runtime */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "recomp_funcs.h"

extern uint8_t *g_base;            /* guest physical memory (1 GB host region) */
extern char g_root[1024];          /* recomp/ folder (orig/, linked/, nand/) */
extern char g_disc[1024];          /* extracted disc (sys/, files/) */

static inline uint8_t *G(uint32_t a) { return g_base + PPC_PHYS(a); }
static inline uint32_t R32(uint32_t a) { return ld32(g_base, a); }
static inline uint16_t R16(uint32_t a) { return ld16(g_base, a); }
static inline uint8_t  R8 (uint32_t a) { return ld8(g_base, a); }
static inline void W32(uint32_t a, uint32_t v) { st32(g_base, a, v); }
static inline void W16(uint32_t a, uint16_t v) { st16(g_base, a, v); }
static inline void W8 (uint32_t a, uint8_t v)  { st8(g_base, a, v); }

/* logging */
extern int g_verbose;
void rt_log(const char *cat, const char *fmt, ...);
void rt_fatal(const char *fmt, ...);
int rt_log_enabled(const char *cat);   /* RT_LOGCAT="di,wm,..." limits verbose logging */
#define LOG(cat, ...) do { if (g_verbose && rt_log_enabled(cat)) rt_log(cat, __VA_ARGS__); } while (0)

/* time: guest timebase runs at 60.75 MHz */
#define TB_HZ 60750000ull
uint64_t rt_now_ns(void);
uint64_t rt_tb_now(void);

/* processor interface interrupts (PI INTSR bits) */
enum {
    PI_GPERR = 1 << 0, PI_RSW = 1 << 1, PI_DI = 1 << 2, PI_SI = 1 << 3, PI_EXI = 1 << 4,
    PI_AI = 1 << 5, PI_DSP = 1 << 6, PI_MEM = 1 << 7, PI_VI = 1 << 8, PI_PE_TOKEN = 1 << 9,
    PI_PE_FINISH = 1 << 10, PI_CP = 1 << 11, PI_DEBUG = 1 << 12, PI_HSP = 1 << 13, PI_WII_IPC = 1 << 14,
};
void pi_set(uint32_t bits, int on);
uint32_t pi_active(void);          /* INTSR & INTMR */

/* hardware */
void hw_init(void);
void hw_update(void);              /* advance VI timing etc.; called from polls */
void ios_init(void);
void ios_mmio_write(uint32_t reg, uint32_t v);
uint32_t ios_mmio_read(uint32_t reg);
void ios_update(void);
void gx_fifo_write(uint32_t v, int size);
void gx_init(void);
uint32_t gx_cp_read(uint32_t reg, int size);
void gx_cp_write(uint32_t reg, uint32_t v, int size);
uint32_t gx_pe_read(uint32_t reg, int size);
void gx_pe_write(uint32_t reg, uint32_t v, int size);

/* disc */
int disc_init(void);
int disc_read(uint64_t part_offset, uint32_t dst, uint32_t len);   /* data-partition space */

/* platform */
typedef struct rt_fiber rt_fiber;
rt_fiber *plat_fiber_main(void);
rt_fiber *plat_fiber_create(void (*fn)(void *), void *arg);
void plat_fiber_switch(rt_fiber *from, rt_fiber *to);
void plat_init(void);
int rt_setting(const char *key, int def);              /* game/settings.ini (key = value) */
void rt_setting_set(const char *key, int val);          /* change + save */
void plat_set_fullscreen(int on); int plat_get_fullscreen(void);
void plat_set_window(int w, int h, int fixed);          /* client size, fixed (non-resizable) */
void gx_set_render_scale(int s);                        /* internal resolution, live */
/* in-game menus (rt_menu.c) */
uint32_t menu_filter_buttons(uint32_t b);               /* raw remote/keyboard buttons -> what the game sees */
void menu_draw(void);                                   /* overlay, called by the renderer before each swap */
void ui_rect(float x, float y, float w, float h, uint32_t rgba);         /* 640x480 virtual screen */
void ui_text(float x, float y, float scale, const char *s, uint32_t rgba);
void ui_image(int tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t rgba);
int  ui_register_texture(const uint32_t *argb, int w, int h);   /* 0xAARRGGBB; returns id for ui_image */
uint32_t *gx_decode_rgba(uint32_t addr, int w, int h, int fmt); /* GX texture -> malloc'd 0xAARRGGBB */
/* game layouts (rt_lyt.c) */
uint32_t lyt_find_pane(uint32_t pane, const char *name);
void lyt_set_alpha(uint32_t pane, uint8_t a); uint8_t lyt_get_alpha(uint32_t pane); void lyt_set_visible(uint32_t pane, int v);
void plat_restart(void);                                /* start a new copy of the game and exit */
void plat_present(const uint32_t *rgba, int w, int h);   /* show a frame */
int  plat_gl_present_begin(int *x, int *y, int *w, int *h, int *cw, int *ch);   /* GL window present: picture rect + client size */
void plat_gl_present_end(void);                          /* swap buffers, FPS counter, events */
int  plat_pump(void);                                    /* process window events; 0 = quit */
uint32_t plat_buttons(void);                             /* keyboard -> Wii Remote buttons */
int  plat_wiimote_present(void);                          /* real Wii Remote connected (Bluetooth HID) */
void plat_wiimote_write(const uint8_t *r, int len);       /* output report to it (id first) */
int  plat_wiimote_read(uint8_t *r, int max);              /* next input report from it, 0 = none */
void plat_audio_push(const int16_t *lr, int frames);     /* 32 kHz stereo, interleaved L,R */
void plat_parallel_for(int n, void (*fn)(int i, void *arg), void *arg);   /* rt_threads.c */

/* trace */
const char *rt_func_name(uint32_t addr, uint32_t *off);
void rt_dump_trace(void);
int rt_widescreen(void);       /* 1 = 16:9 mode (aspect_4_3 = 0) */
void gx_set_filter(int mode);   /* 0 nearest, 1 bilinear, 2..5 anisotropic 2x..16x */
uint32_t *ui_load_png(const char *path, int *w, int *h);   /* 0xAARRGGBB, malloc'd */
int wm_status(void);           /* 0 no remote channel yet, 1 emulated (keyboard), 2 real Wii Remote */
