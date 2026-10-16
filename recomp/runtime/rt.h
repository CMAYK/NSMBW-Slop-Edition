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
/* deterministic mode (online play: rt_core.c virtual clock, rt_det.c) */
extern int g_det;
extern uint64_t g_vt_ns;
uint64_t rt_emu_ns(void);                                  /* the emulated hardware's clock: virtual in det mode */
void det_field(void);                                      /* rt_det.c: a new video field (inputs, checks) */
uint32_t det_keyboard(void);                               /* the keyboard, as latched for this field */
uint32_t det_rtc_secs(void);
void det_renderer_writes(uint32_t phys, uint32_t bytes);  /* memory the renderer writes (not hashed) */
uint64_t det_state_hash(void);                             /* the game's state, without what the renderer writes */
int  net_session_det(void);                                /* this run is an online session: deterministic */
const char *rt_nand_root(void);                            /* <game>/nand, or <game>/online/nand in a session */
int  session_slot_active(int slot);                        /* online session: a player is in this slot */
uint32_t session_slot_buttons(int slot);                   /* ... and its buttons this field */
int  session_on(void);
uint32_t session_rtc(void);
const char *rt_session_dir(void);                          /* <game>/online/<name> in a session, else NULL */
void plat_title_note(const char *note);                    /* shown in the window title (NULL: none) */
int  net_start_session(void);                              /* host, in the lobby: start the online game */
void net_leave_session(void);
void rt_det_enable(void);                                  /* virtual clock from now on (rt_core.c) */
void rt_settings_reload(void);
int  net_online_starting(void);                            /* host: the online game is being started */
int  net_online_players(void);
const char *net_notice(void);                               /* why the last online game ended (a few seconds) */                             /* host: players joined */
int  net_snap_pending(void);                               /* rt_net.c: the online game starts at this field */
void net_snap_point(void);
void net_shutdown(void);                                   /* the game is closing */
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
void hw_update(void);
/* rt_si.c - GameCube controller ports; rt_remotes.c - the players' controllers */
uint32_t si_read_sized(uint32_t r, int size); void si_write_sized(uint32_t r, uint32_t v, int size); void si_tick(void);
int remotes_gc_state(int port, uint8_t s[8]);   /* GC port 0-3 (players 5-8): 1 + its 8 poll bytes, 0 = empty */              /* advance VI timing etc.; called from polls */
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
/* save states (rt_state.c). The variables of a file with RT_STATE_FILE (after its #includes)
   are part of a save state, except those marked RT_LOCAL (this process's own: handles, host
   pointers, the PC's clock, renderer caches); RT_STATE puts a variable of another file in it. */
#if !defined(__clang__)
#error "the runtime is built with clang (save state sections: #pragma clang section)"
#endif
#ifdef _WIN32
#define RT_STATE_FILE _Pragma("clang section bss=\"rtb\" data=\"rtd\"")
#define RT_STATE __attribute__((section("rtd")))
#else
#define RT_STATE_FILE _Pragma("clang section bss=\"rts_b\" data=\"rts_d\"")
#define RT_STATE __attribute__((section("rts_d")))
#endif
#define RT_LOCAL __attribute__((section("rtl")))
typedef struct rt_snap rt_snap;
uint8_t *rt_layout_init(void);                             /* guest memory + arena at fixed addresses */
int  rt_state_ok(void);                                    /* save states possible (fixed layout) */
uint8_t *rt_aram(void);                                    /* ARAM (16 MB) */
rt_snap *rt_snap_new(void);
void rt_snap_free(rt_snap *s);
int  rt_snap_take(rt_snap *s);                             /* 0 taken, 1 resumed by a load, -1 can't */
void rt_snap_load(rt_snap *s);                             /* doesn't return (resumes where it was taken) */
const uint8_t *rt_snap_data(rt_snap *s, size_t *n);        /* the state as bytes (to send) */
int  rt_snap_set(rt_snap *s, const uint8_t *p, size_t n);  /* ... and back (0: not a state of this build) */
typedef struct rt_fiber rt_fiber;
void rt_state_host_loop(rt_fiber *guest_main);
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
void plat_idle_wait(uint64_t ns);                           /* sleep up to ns (woken early by events) */
int  plat_pump(void);                                    /* process window events; 0 = quit */
uint32_t plat_buttons(void);                             /* keyboard -> Wii Remote buttons */
/* real Wii Remotes on the PC (rt_plat_win.c: Bluetooth HID; Linux: RT_FAKE_WM for tests).
   Each connected remote has an id (good while it stays connected) and its Bluetooth address
   (0 = unknown). Reports: id byte first. */
typedef struct { int id; uint64_t addr; } plat_wm_dev;
int  plat_wm_list(plat_wm_dev *out, int max);              /* the remotes connected now */
int  plat_wm_read(int id, uint8_t *r, int max);            /* next input report, 0 = none */
void plat_wm_write(int id, const uint8_t *r, int len);     /* output report */
/* pairing (Options > Controls): looks for remotes in pairing mode (red SYNC button) and pairs
   them with the PC for good - the remote remembers the PC and reconnects with any button */
int  plat_wm_pair_supported(void);
void plat_wm_pair(int on);                                 /* start / stop looking */
int  plat_wm_pair_state(char *msg, int n);                 /* 0 idle, 1 looking, 2 pairing one, 3 paired one (msg), -1 failed (msg) */
int  plat_wm_forget(uint64_t addr);                        /* the PC forgets it (unpair); 1 = done */
uint64_t plat_wm_pair_last(unsigned *gen);                 /* address of the remote paired last (gen counts pairings) */
/* rt_remotes.c: the 8 player slots (1-4 Wii Remote channels, 5-8 GameCube ports): keyboard, a remote, or none */
enum { REMOTES_NONE, REMOTES_KEYBOARD, REMOTES_REMOTE, REMOTES_ONLINE };
void remotes_update(void);
int  remotes_slot_dev(int slot);                           /* remote device in slot 0-7, -1 = none */
int  remotes_slot_keyboard(int slot);                      /* the keyboard is this slot's */
uint32_t remotes_game_buttons(int slot, uint32_t device);  /* slot's buttons for the game (device = its remote's) */
int  remotes_read(int dev, uint8_t *r, int max);
void remotes_write(int dev, const uint8_t *r, int len);    /* (puts the player lights into LED reports) */
int  remotes_player_of_slot(int slot);                     /* the module's player 0-7 for it, -1 = not in yet */
uint8_t remotes_lights_for(int player);                    /* LED pattern, bit 0 = light 1 */
void remotes_slot_info(int slot, int *kind, uint64_t *addr, int *connected, int *player);
int  remotes_addr_connected(uint64_t addr);
int  remotes_unassigned(uint64_t *addrs, int max);         /* connected remotes without a slot */
int  remotes_choices(int *kinds, uint64_t *addrs, int max);/* none, keyboard, known remotes */
int  remotes_assign(int slot, int kind, uint64_t addr);    /* (swaps with the slot that had it) */
int  remotes_slot_emulated(int slot);                      /* keyboard or online: an emulated remote */
int  remotes_net_take(int count, int *slots);              /* host: slots for an online player's controllers */
void remotes_net_release(int slot);
int  remotes_local_players(void);                          /* client: this PC's players */
uint32_t remotes_local_buttons(int player);
int remotes_local_slot(int player);
uint32_t remotes_slot_raw(int slot);                       /* slot's own controller's buttons now */
/* profiles (rt_profiles.c) */
int  prof_count(void);
const char *prof_name(int i);
const char *prof_id(int i);
const char *prof_pref(int i);
void prof_stats(int i, uint32_t *cleared, uint32_t *deaths, uint32_t *coins);
int  prof_find_name(const char *name);
int  prof_create(const char *name, const char *pref);
int  prof_edit(int i, const char *name, const char *pref);
int  prof_of_controller(int slot);
void prof_set_controller(int slot, int i);
int  prof_of_slot(int slot);
void prof_set_slot(int slot, int i);
void prof_local_player(int k, char name[5], int *pref);
/* characters (rt_chars.c) */
const char *chars_id(int c);
int  chars_index(const char *id);
void chars_set_slot_profile(int slot, const char *name, int pref);
const char *chars_tag_of_player(int n);
const char *chars_tag_of_slot(int slot);                        /* the slot this PC's player n is in (-1 none) */
/* rt_net.c: online play (docs/ONLINE.md) */
enum { NET_OFF, NET_HOST, NET_JOINING, NET_CLIENT };
void net_update(void);
int  net_host(int port);
int  net_join(const char *addr);                           /* "ip:port" or "name:port" */
void net_leave(void);
int  net_role(void);
const char *net_status(void);
int  net_port(void);
uint32_t net_slot_buttons(int slot);
const char *net_slot_name(int slot);
int  net_slot_ping(int slot);
int  net_lobby(int i, char *name, int n, int *slot, int *ping);
int  plat_text_char(void);                                 /* a typed character (0: none) */
void remotes_forget(int slot);
int  wiimote_connected(int i);                              /* rt_wiimote.c: game remote i is in */
int  menu_owns_input(void);                                 /* rt_menu.c: our menus (or the title) have the controls */
extern uint64_t g_frames_presented;
void sfx_play(const char *dir, const char *name);         /* rt_sfx.c: ui/retro/<dir>/sfx/<name>.wav */
void sfx_stop_all(void);
void sfx_mix(int16_t *pcm, int frames);                   /* (into a 32 kHz stereo block) */
void plat_audio_push(const int16_t *lr, int frames);     /* 32 kHz stereo, interleaved L,R */
void plat_parallel_for(int n, void (*fn)(int i, void *arg), void *arg);   /* rt_threads.c */

/* trace */
const char *rt_func_name(uint32_t addr, uint32_t *off);
void rt_dump_trace(void);
int rt_widescreen(void);       /* 1 = 16:9 mode (aspect_4_3 = 0) */
void gx_set_filter(int mode);   /* 0 nearest, 1 bilinear, 2..5 anisotropic 2x..16x */
uint32_t *ui_load_png(const char *path, int *w, int *h);   /* 0xAARRGGBB, malloc'd */
int wm_status(void);           /* 0 no remote channel yet, 1 emulated (keyboard), 2 real Wii Remote */
/* the 8-player module's asset archive: mod/<name> on the host, /<name> in the game's file system */
#define MOD_ARC_NAME "nsmbw-8p.arc"
/* call a guest function from native code: fn(args...) with up to 8 integer arguments in r3..r10;
   returns r3. Volatile registers, lr and ctr are kept. (rt_gamesetup.c) */
uint32_t rt_gcall_(const char *file, int line, ppc_context *ctx, uint8_t *base, uint32_t fn, int n, ...);
#define rt_gcall(...) rt_gcall_(__FILE__, __LINE__, __VA_ARGS__)
/* give a native function a guest address, so guest code can call it (function pointers, vtables) */
void rt_register_native(uint32_t addr, ppc_func fn);
void rt_post_callback(uint32_t fn, uint32_t a0, uint32_t a1);   /* rt_core.c: a guest callback "from an interrupt" (track A) */
int  rt_events_pending(void);

/* the character list (rt_chars.c, docs/CHARACTERS.md) */
int chars_count(void);
const char *chars_name(int c);
int chars_of_player(int player);          /* roster index of player slot 0-7, -1 none */
int chars_style_of_player(int player);    /* 0 normal, 1 SMB1, 2 SMW, 3 SM64 */
int chars_is_smb1(int player);
void chars_keep_choice(int player);        /* it took its character back: the card opens on it again */
int chars_select_facing(int player);       /* character select: -1 / 1 the way it last pressed left / right */
void chars_title_players(void);           /* title screen: the first five characters */


/* hashes and signatures (rt_crypto.c) */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
int  ed25519_verify(const uint8_t sig[64], const void *msg, size_t n, const uint8_t pk[32]);   /* 1 = good */

/* updates (rt_update.c, docs/UPDATES.md) */
const char *rt_version(void);              /* this build's version ("0.1.0") */
int  rt_version_cmp(const char *a, const char *b);   /* <0, 0, >0 (dotted numbers) */
void update_install_pending(void);         /* at start: put the files the last run downloaded in place */
void update_begin(void);                   /* at start: look for an update in the background */
int  update_boot_hold(void);               /* the boot waits on the update screen (title not started yet) */
enum { UPD_IDLE, UPD_CHECKING, UPD_LATEST, UPD_NO_SERVER, UPD_DOWNLOADING, UPD_INSTALLING, UPD_RESTART, UPD_FAILED, UPD_SKIPPED, UPD_OFF };
int  update_status(char *msg, int cap, int64_t *got, int64_t *total, double *bytes_per_s, char *new_version, int vcap);
void update_skip(void);                    /* the player skips the download */
int  update_screen_active(void);           /* the boot is waiting on the update screen */
void update_screen_times(double *t0, double *t_end);   /* (wall-clock seconds: shown, finished) */

/* rollback states: one per video field, kept cheaply (rt_state.c; online play) */
int  rb_start(void);                       /* 1 = on (from the current state) */
void rb_stop(void);
int  rb_active(void);
int  rb_supported(void);
int  rb_save(uint32_t field, int keep);            /* at the start of a field: 0 kept, 1 = a rollback resumed here, -1 off */
int  rb_can_go_back(uint32_t field);
uint32_t rb_newest(void);
void rb_rollback(uint32_t field);          /* back to that field's state (doesn't return) */
void rb_stats(double *save_ms, uint64_t *saves, uint64_t *skips);
size_t rt_fread_guest(void *dst, size_t n, FILE *f);   /* fread into guest memory (Linux: pages may be read-only) */
extern int g_resim;                        /* running fields again after a rollback / catching up: nothing drawn or heard */
