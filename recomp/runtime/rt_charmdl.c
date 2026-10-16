/* rt_charmdl.c - models for the characters that don't have one in the game (docs/CHARACTERS.md).
 *
 * The models come from mods the user put in game/mod/ (tools/import_char_mods.py extracts them
 * to game/mod/characters/<set>/<name>.brres). Each character is still one of the game's player types
 * (its "body", rt_chars.c); here the model a body shows is replaced:
 *
 *   toad/model.brres            all Toad bodies (blue, yellow, black): the Toad addon's Kinopio
 *                               model, whose Black Toad textures are Toadsworth's
 *   rosalina_daisy/model.brres  Daisy on the Orange Toad body, Rosalina on the Purple Toadette
 *   + P_rcha.brres, K_rcha.brres  body (made a Toad-type model): that mod's Kinopio model -
 *                               Daisy in the blue Toad's texture frames, Rosalina in the yellow
 *                               one's - with its own animations (dress / skirt bones)
 *
 * Hooks:
 *   mod_fn_800D6D10  dPyMdlMng_c::construct    - Purple Toadette's body gets a Toad-type model
 *   fn_800D32F0      dPlayerMdl_c::loadResources - its ResFiles (model, P_rcha, own anims) are
 *                                                 pointed at ours
 *   mod_fn_800BCEE0  dKinopioMdl_c::setColorType - texture frames as the mod expects them
 * The brres files are loaded once into guest memory at 0x81C00000+ (below the renderer's
 * texture limit) and set up like the game's (ResFile::Init + Bind). */
#include "rt.h"
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

#define MDL_MEM          0x81C00000u
#define MDL_MEM_END      0x82000000u
#define MM_SCRATCH       (MDL_MEM_END - 0x100)
#define A_RESFILE_INIT   0x8023A590u   /* nw4r::g3d::ResFile::Init() */
#define A_RESFILE_BIND   0x8023A350u   /* nw4r::g3d::ResFile::Bind(ResFile) */
/* model types (dPyMdlMng_c::ModelType_e) */
enum { MT_MARIO, MT_LUIGI, MT_TOAD_BLUE, MT_TOAD_YELLOW, MT_TOAD_RED, MT_YOSHI, MT_TOADETTE, MT_TOADETTE_PURPLE,
       MT_TOAD_BLACK, MT_TOAD_ORANGE };
/* dPlayerMdl_c */
#define PM_CHARA_ID      0x151
#define PM_RES_MODEL     0x20C
#define PM_RES_COMMON    0x210         /* P_rcha */
#define PM_RES_ANIM      0x214         /* K_rcha (Toads) */
#define MNG_MODEL        0x004         /* dPyMdlMng_c::mModel */

#define gcall rt_gcall

typedef struct { const char *file; int state; uint32_t addr; } brres_t;   /* state 0 untried, 1 ok, -1 none */
enum { F_TOAD, F_RD_MODEL, F_RD_PRCHA, F_RD_KRCHA, F_COUNT };
static brres_t files[F_COUNT] = {
    {"toad/model.brres"}, {"rosalina_daisy/model.brres"}, {"rosalina_daisy/P_rcha.brres"}, {"rosalina_daisy/K_rcha.brres"},
};
static uint32_t mem_next = MDL_MEM;

static uint32_t brres(ppc_context *ctx, uint8_t *base, int i) {
    brres_t *f = &files[i];
    if (f->state) return f->state > 0 ? f->addr : 0;
    f->state = -1;
    char path[1200]; snprintf(path, sizeof path, "%s/mod/characters/%s", g_root, f->file);
    FILE *fp = fopen(path, "rb"); if (!fp) return 0;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint32_t at = (mem_next + 31) & ~31u;
    if (n <= 16 || at + (uint32_t)n > MM_SCRATCH) { fclose(fp); rt_log("chars", "%s: no room", path); return 0; }
    size_t got = rt_fread_guest(G(at), (size_t)n, fp); fclose(fp);
    if (got != (size_t)n || memcmp(G(at), "bres", 4)) { rt_log("chars", "%s: not a brres", path); return 0; }
    mem_next = at + (uint32_t)n;
    /* as the game's resource loader does: ResFile::Init(), then Bind() to itself */
    W32(MM_SCRATCH, at); W32(MM_SCRATCH + 4, at);
    gcall(ctx, base, A_RESFILE_INIT, 1, MM_SCRATCH);
    gcall(ctx, base, A_RESFILE_BIND, 2, MM_SCRATCH, MM_SCRATCH + 4);
    f->addr = at; f->state = 1;
    rt_log("chars", "model file %s: %ld KB at %08X", f->file, n / 1024, at);
    return at;
}
static int have_rd(ppc_context *ctx, uint8_t *base) {
    return brres(ctx, base, F_RD_MODEL) && brres(ctx, base, F_RD_PRCHA) && brres(ctx, base, F_RD_KRCHA);
}

/* dPyMdlMng_c::construct(model type): Purple Toadette's body becomes a Toad-type model when
   Rosalina's model is there (it is built as an Orange Toad model, then given its own id) */
void hle_mod_fn_800D6D10(ppc_context *ctx, uint8_t *base) {
    uint32_t mng = ctx->r[3]; int type = (int)(ctx->r[4] & 0xFF);
    if (type != MT_TOADETTE_PURPLE || !have_rd(ctx, base)) { orig_mod_fn_800D6D10(ctx, base); return; }
    ctx->r[4] = MT_TOAD_ORANGE;
    orig_mod_fn_800D6D10(ctx, base);
    uint32_t mdl = R32(mng + MNG_MODEL);
    if (mdl) W8(mdl + PM_CHARA_ID, MT_TOADETTE_PURPLE);
}

/* dPlayerMdl_c::loadResources */
void hle_fn_800D32F0(ppc_context *ctx, uint8_t *base) {
    uint32_t mdl = ctx->r[3];
    orig_fn_800D32F0(ctx, base);
    int id = R8(mdl + PM_CHARA_ID);
    if ((id == MT_TOAD_ORANGE || id == MT_TOADETTE_PURPLE) && have_rd(ctx, base)) {
        /* (only Toad-type models get here as purple: see construct) */
        W32(mdl + PM_RES_MODEL, files[F_RD_MODEL].addr);
        W32(mdl + PM_RES_COMMON, files[F_RD_PRCHA].addr);
        W32(mdl + PM_RES_ANIM, files[F_RD_KRCHA].addr);
    } else if ((id == MT_TOAD_BLUE || id == MT_TOAD_YELLOW || id == MT_TOAD_BLACK) && brres(ctx, base, F_TOAD)) {
        W32(mdl + PM_RES_MODEL, files[F_TOAD].addr);
    }
}

/* dKinopioMdl_c::setColorType: the mod's texture frames are the blue Toad's (Daisy) and the
   yellow Toad's (Rosalina) */
void hle_mod_fn_800BCEE0(ppc_context *ctx, uint8_t *base) {
    uint32_t mdl = ctx->r[3];
    int id = R8(mdl + PM_CHARA_ID);
    if ((id == MT_TOAD_ORANGE || id == MT_TOADETTE_PURPLE) && files[F_RD_MODEL].state > 0
        && R32(mdl + PM_RES_MODEL) == files[F_RD_MODEL].addr) {
        W8(mdl + PM_CHARA_ID, id == MT_TOAD_ORANGE ? MT_TOAD_BLUE : MT_TOAD_YELLOW);
        orig_mod_fn_800BCEE0(ctx, base);
        W8(mdl + PM_CHARA_ID, (uint8_t)id);
        return;
    }
    orig_mod_fn_800BCEE0(ctx, base);
}
