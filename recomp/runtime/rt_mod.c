/* rt_mod.c - the 8-player module (8 players and more; THIRD_PARTY_NOTICES.md), built into the
 * recompiled game while it is moved to native C (NATIVE_PORT.md).
 *
 * On a Wii the module is a REL that a small loader reads from its archive, links into a heap block
 * and starts from the boot scene; its _prolog then patches the game's code. Here all of that
 * linking and patching happened at build time (tools/link_mod.py, mod_patches.py, recomp.py), so
 * at runtime only the module's memory image is placed at its link address and its start-up code
 * is run at the moment the loader would have run it (the boot scene's step at 0x80328130, which
 * the Riivolution patch points at the loader instead of fn_8015BB20 "return true").
 *
 * The module's assets come from its archive (MOD_ARC_NAME), added to the disc's file system by
 * rt_disc.c.
 * Its settings are bytes at 0x80006784.. (alignment padding in .text) that Riivolution options
 * set; here they come from settings.ini. */
#include "rt.h"
#include "recomp_mod.h"
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

int g_mod_on;                             /* switches the game's code to the patched version */
#if MOD_PRESENT
#define MOD_SCRATCH  0x81860000u          /* past the module's .bss: loader leftovers live here */
#define A_OSGetTick                  0x801B5FA0u
#define A_DVDConvertPathToEntrynum   0x801CA680u
#define A_ARCInitHandle              0x8019F660u

static void *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *d = malloc(n ? n : 1); if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f); if (len) *len = n; return d;
}

const char *mod_arc_path(void) {
    static char p[1200];
    if (!p[0]) snprintf(p, sizeof p, "%s/mod/" MOD_ARC_NAME, g_root);
    return p;
}

static const struct { const char *key; uint32_t addr; int def; } mod_flags[] = {
    { "unlock_all_worlds",           0x80006784u, 0 },
    { "complete_all_levels",         0x80006785u, 0 },
    { "no_powerup_pause",            0x80006786u, 0 },
    { "infinite_lives",              0x80006787u, 0 },
    { "death_messages",              0x80006788u, 0 },   /* 1 all damage, 2 deaths only */
    { "fall_damage",                 0x80006789u, 0 },
    { "skip_opening_movie",          0x8000678Au, 0 },
    { "always_exit_course",          0x8000678Bu, 0 },
    { "shake_with_button",           0x8000678Cu, 0 },
    { "hide_motion_guides",          0x8000678Du, 1 },   /* 1 with 5+ players, 2 always */
    { "yoshi_colors",                0x8000678Eu, 0 },   /* 1 random, 2 all green */
    { "liftable_tiles",              0x8000678Fu, 0 },
    { "bubble_swarm",                0x80006794u, 0 },
    { "deadly_1up",                  0x80006795u, 0 },
    { "all_paths_open",              0x80006796u, 0 },
};


/* ---- save data: the module keeps saves as JSON (nsmbw-save.json, bigger structures for 8
   players) instead of wiimj2d.sav. The first time, the original save is converted so progress
   carries over (same format the module writes: d_mj2d_json_handler.cpp). The original file is
   left untouched. */
static const char *st_names[42] = { "1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16","17","18","19",
    "coin","ghost_house","tower","tower_2nd","castle","castle_2nd","toad_house_1","toad_house_2","toad_house_3",
    "toad_house_4","toad_house_5","toad_house_6","toad_house_7","ambush_1","ambush_2","ambush_3","cannon","37",
    "airship","rescue","title","peach_castle","staff_roll" };
static const char *pl_names[4] = { "mario", "luigi", "blue_toad", "yellow_toad" };
static const char *mode_names[7] = { "small", "mushroom", "fire", "mini", "propeller", "penguin", "ice" };
static const char *stock_names[7] = { "mushroom", "fire_flower", "propeller_shroom", "ice_flower", "penguin_suit", "mini_mushroom", "star" };
static const char *kinoko_names[7] = { "none", "yellow", "red", "green", "yellow_r", "red_r", "green_r" };
static const char *dir_names[3] = { "normal", "reverse", "initial" };
static uint16_t sv16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t sv32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void conv_game(FILE *o, const uint8_t *g, const char *key) {
    fprintf(o, ",\"%s\":{\"version\":\"14.1\",\"completion\":[\"created\"", key);
    static const char *cflags[] = { NULL, "final_boss_beaten", "all_goals", "all_star_coins", "all_star_coins_w9", "completed", "super_gude_triggered" };
    for (int b = 1; b < 7; b++) if (g[2] & (1 << b)) fprintf(o, ",\"%s\"", cflags[b]);
    fprintf(o, "],\"score\":%u,\"staff_roll_high_score\":%u,\"current_world\":%u,\"current_sub_world\":%u,\"path_node\":%u",
            sv32(g + 0x68), sv16(g + 0x66), g[3] + 1, g[4], g[5]);
    fprintf(o, ",\"stock_item\":{"); int first = 1;
    for (int i = 0; i < 7; i++) if (g[9 + i]) { fprintf(o, "%s\"%s\":%u", first ? "" : ",", stock_names[i], g[9 + i]); first = 0; }
    fprintf(o, "},\"hint_movie_bought\":["); first = 1;
    for (int i = 0; i < 70; i++) if (g[0x6FC + i]) { fprintf(o, "%s%d", first ? "" : ",", i); first = 0; }
    fprintf(o, "],\"player\":{");
    for (int t = 0; t < 4; t++) {
        int num = 8; for (int i = 0; i < 4; i++) if (g[0x2A + i] == t) { num = i; break; }
        fprintf(o, "%s\"%s\":{\"lives\":%u,\"coins\":%d,\"continues\":%u,\"equip\":[%s],\"powerup\":\"%s\",\"player\":%d}",
                t ? "," : "", pl_names[t], g[0x22 + t], (int8_t)g[0x1E + t], g[0x1A + t], (g[0x26 + t] & 1) ? "\"star_power\"" : "",
                mode_names[g[0x2E + t] < 7 ? g[0x2E + t] : 0], num);
    }
    fprintf(o, "},\"world\":{");
    for (int w = 0; w < 10; w++) {
        uint8_t kc = g[0x742 + w];
        fprintf(o, "%s\"%d\":{\"open\":%s,\"toad_rescue_course\":\"%s\",\"start_minigame_type\":\"%s\",\"ambush_enemy\":{",
                w ? "," : "", w + 1, (g[0x32 + w] & 1) ? "true" : "false", kc < 42 ? st_names[kc] : "none", kinoko_names[g[0x10 + w] < 7 ? g[0x10 + w] : 0]);
        first = 1;
        for (int e = 0; e < 4; e++) {
            uint8_t rc = g[0x3C + w * 4 + e], sw = g[0x74C + w * 4 + e], dir = g[0x79C + w * 4 + e]; int8_t pn = (int8_t)g[0x774 + w * 4 + e];
            if (!rc && !sw && pn == -1 && dir == 2) continue;
            fprintf(o, "%s\"%d\":{\"revival_count\":%u,\"sub_world\":%u,\"path_node\":%d,\"walk_direction\":\"%s\"}", first ? "" : ",", e, rc, sw, pn, dir_names[dir < 3 ? dir : 2]);
            first = 0;
        }
        fprintf(o, "},\"course\":{"); first = 1;
        for (int st = 0; st < 42; st++) {
            uint32_t c = sv32(g + 0x6C + (w * 42 + st) * 4);
            uint8_t d = g[0x7C4 + w * 42 + st];
            int sw = w == 2 && st == 3;
            uint32_t def = st == 38 ? 0x30 : 0;
            if ((c & 0x1B7) == def && !d && !(sw && g[0x968])) continue;
            fprintf(o, "%s\"%s\":{\"goal\":[", first ? "" : ",", st_names[st]); first = 0;
            int f2 = 1; const char *gn[4] = { "normal", "secret", "super_guide_normal", "super_guide_secret" }; const int gb[4] = { 4, 5, 7, 8 };
            for (int k = 0; k < 4; k++) if (c & (1u << gb[k])) { fprintf(o, "%s\"%s\"", f2 ? "" : ",", gn[k]); f2 = 0; }
            fprintf(o, "],\"star_coin\":["); f2 = 1;
            for (int k = 0; k < 3; k++) if (c & (1u << k)) { fprintf(o, "%s%d", f2 ? "" : ",", k + 1); f2 = 0; }
            fprintf(o, "],\"deaths\":%u", d);
            if (sw) fprintf(o, ",\"deaths_switch\":%u", g[0x968]);
            fprintf(o, "}");
        }
        fprintf(o, "}}");
    }
    fprintf(o, "}}");
}

static void convert_save(void) {
    char dir[1200], src[1300], dst[1300];
    snprintf(dir, sizeof dir, "%s/title/00010000/534d4e45/data", rt_nand_root());
    snprintf(dst, sizeof dst, "%s/nsmbw-save.json", dir);
    FILE *t = fopen(dst, "rb"); if (t) { fclose(t); return; }
    snprintf(src, sizeof src, "%s/wiimj2d.sav", dir);
    size_t n; uint8_t *d = read_all(src, &n);
    if (!d) return;
    if (n < 0x6A0 + 6 * 0x980) { free(d); rt_log("mod", "wiimj2d.sav too small to convert"); return; }
    FILE *o = fopen(dst, "wb"); if (!o) { free(d); return; }
    fprintf(o, "{\"version\":\"14.1\",\"last_selected_file\":%u", d[8]);
    const char *pc[2] = { "play_count_free_mode", "play_count_coin_battle" };
    for (int k = 0; k < 2; k++) {
        fprintf(o, ",\"%s\":{", pc[k]); int first = 1;
        for (int w = 0; w < 10; w++) for (int st = 0; st < 42; st++) {
            uint16_t v = sv16(d + (k ? 0x350 : 0x00A) + (w * 42 + st) * 2);
            if (v) { fprintf(o, "%s\"%d-%s\":%u", first ? "" : ",", w + 1, st_names[st], v); first = 0; }
        }
        fprintf(o, "}");
    }
    fprintf(o, ",\"available_worlds\":["); int first = 1;
    for (int w = 0; w < 10; w++) if (sv16(d + 0x698) & (1 << w)) { fprintf(o, "%s%d", first ? "" : ",", w + 1); first = 0; }
    fprintf(o, "]");
    int files = 0;
    for (int s = 0; s < 6; s++) {
        const uint8_t *g = d + 0x6A0 + s * 0x980;
        if (g[2] & 1) continue;                               /* empty slot */
        char key[16]; snprintf(key, sizeof key, "%s%d", s < 3 ? "file" : "temp", s % 3 + 1);
        conv_game(o, g, key); files++;
    }
    fprintf(o, "}");
    fclose(o); free(d);
    rt_log("save", "converted wiimj2d.sav (%d files) to nsmbw-save.json", files);
}

/* place the linked image (load_dol has already put the game in memory) */
void mod_load(void) {
    char p[1200]; size_t n;
    snprintf(p, sizeof p, "%s/orig/d_project.bin", g_root);
    uint8_t *img = read_all(p, &n);
    if (!img) { snprintf(p, sizeof p, "%s/linked/d_project.bin", g_root); img = read_all(p, &n); }   /* dev layout */
    if (!img || n != MOD_IMAGE_SIZE) rt_fatal("%s is missing or doesn't match this build", p);
    memcpy(G(MOD_LOAD), img, n); free(img);
    memset(G(MOD_BSS), 0, MOD_BSS_SIZE);
    FILE *f = fopen(mod_arc_path(), "rb");
    if (!f) rt_fatal("%s is missing (the 8-player mod's assets)", mod_arc_path());
    fclose(f);
    for (unsigned i = 0; i < sizeof mod_flags / sizeof *mod_flags; i++)
        W8(mod_flags[i].addr, (uint8_t)rt_setting(mod_flags[i].key, mod_flags[i].def));
    rt_log("mod", "d_project module at %08X (%u bytes)", MOD_LOAD, (unsigned)n);
    convert_save();
}

static uint32_t gcall(ppc_context *ctx, uint8_t *base, uint32_t fn, uint32_t a, uint32_t b) {
    uint32_t lr = ctx->lr;
    ctx->r[3] = a; ctx->r[4] = b; ctx->lr = 0;
    ppc_call_indirect(ctx, base, fn);
    ctx->lr = lr;
    return ctx->r[3];
}

/* what the loader + _prolog do once the module is in memory, minus the code patching */
static void mod_start(ppc_context *ctx, uint8_t *base) {
    uint32_t saved[32]; memcpy(saved, ctx->r, sizeof saved);
    g_mod_on = 1;                                         /* = _prolog's code patches */
    gcall(ctx, base, MOD_STACK_CHK_INIT, gcall(ctx, base, A_OSGetTick, 0, 0), 0);
    W32(MOD_CODE_REGION_VAR, gcall(ctx, base, MOD_FIND_CODE_REGION, 0, 0));
    /* the archive handle the loader leaves for the module's DVD extension */
    const uint32_t name = MOD_SCRATCH, handle = MOD_SCRATCH + 0x20, hdr = MOD_SCRATCH + 0x100;
    memcpy(G(name), MOD_ARC_NAME, sizeof MOD_ARC_NAME);
    int32_t entry = (int32_t)gcall(ctx, base, A_DVDConvertPathToEntrynum, name, 0);
    if (entry < 0) rt_fatal(MOD_ARC_NAME " is not in the disc file system");
    size_t n; uint8_t *arc = read_all(mod_arc_path(), &n);
    if (!arc || n < 0x20) rt_fatal("cannot read %s", mod_arc_path());
    uint32_t file_start = ((uint32_t)arc[12] << 24 | arc[13] << 16 | arc[14] << 8 | arc[15]);
    uint32_t hsize = (file_start + 31) & ~31u;
    if (hsize > 0x10000 - 0x100 || hsize > n) rt_fatal(MOD_ARC_NAME " header too large (%u)", hsize);
    memcpy(G(hdr), arc, hsize); free(arc);
    if (!gcall(ctx, base, A_ARCInitHandle, hdr, handle)) rt_fatal("ARCInitHandle failed for " MOD_ARC_NAME);
    rt_log("mod", MOD_ARC_NAME ": entry %d, header %u bytes", entry, hsize);
    if (gcall(ctx, base, MOD_PREINIT, (uint32_t)entry, handle) != 0) rt_fatal("module preinit failed");
    for (unsigned i = 0; i < sizeof mod_ctors / sizeof *mod_ctors; i++) gcall(ctx, base, mod_ctors[i], 0, 0);
    if (gcall(ctx, base, MOD_MAIN, 0, 0) != 0) rt_fatal("module main failed");
    memcpy(ctx->r, saved, sizeof saved);
    rt_log("mod", "module started");
}

/* For checks on who called something: an address inside a module function that replaces a game
   function maps to that game function's start; anything else is returned as is. */
uint32_t mod_game_addr(uint32_t a) {
    if (!g_mod_on || a < MOD_LOAD || a >= MOD_BSS) return a;
    int lo = 0, hi = (int)(sizeof mod_replace_map / sizeof *mod_replace_map);
    while (lo < hi) { int m = (lo + hi) / 2; if (mod_replace_map[m][0] <= a) lo = m + 1; else hi = m; }
    if (lo && a < mod_replace_map[lo - 1][1]) return mod_replace_map[lo - 1][2];
    return a;
}

/* the boot scene's step that the loader replaces ("return true" in the original game) */
void hle_fn_8015BB20(ppc_context *ctx, uint8_t *base) {
    static int started;
    if (!started) { started = 1; mod_start(ctx, base); }
    ctx->r[3] = 1;
}
#else
uint32_t mod_game_addr(uint32_t a) { return a; }
void mod_load(void) {}
const char *mod_arc_path(void) { return NULL; }
void hle_fn_8015BB20(ppc_context *ctx, uint8_t *base) { orig_fn_8015BB20(ctx, base); }
#endif
