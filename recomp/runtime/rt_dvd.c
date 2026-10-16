/* rt_dvd.c - the disc, native (track A3, docs/NATIVE_ROADMAP.md).
 *
 * The SDK's DVD library sends every read to the drive through IOS (/dev/di) and finishes it from
 * the IPC interrupt. Here a read is a copy from the disc image in memory (rt_disc.c) done at
 * once; the command block is marked busy and its callback runs "from an interrupt" a moment
 * later (rt_post_callback), as the game expects. Synchronous reads (DVDReadPrio) just copy.
 *
 * DVDCommandBlock: +0x08 command, +0x0C state (0 end, 1 busy, -1 fatal, 10 canceled),
 * +0x10 offset (in 4-byte words), +0x14 length, +0x18 address, +0x20 transferred, +0x28 callback.
 * DVDFileInfo = block + +0x30 start (words), +0x34 length, +0x38 user callback. */
#include "rt.h"
RT_STATE_FILE                                   /* (this file's variables are part of a save state: rt_state.c) */

enum { CB_COMMAND = 0x08, CB_STATE = 0x0C, CB_OFFSET = 0x10, CB_LENGTH = 0x14, CB_ADDR = 0x18, CB_TRANSFERRED = 0x20, CB_CALLBACK = 0x28 };
enum { ST_END = 0, ST_BUSY = 1, ST_CANCELED = 10 };
#define DVD_DONE_FN 0x80001F10u                 /* guest address of dvd_done (rt_register_native) */
uint64_t g_dvd_reads RT_LOCAL, g_dvd_bytes RT_LOCAL;
static uint32_t pending[64]; static int npending;          /* blocks with our completion still queued */
static int pending_find(uint32_t b) { for (int i = 0; i < npending; i++) if (pending[i] == b) return i; return -1; }

static void do_read(uint32_t addr, uint32_t len, uint32_t offset_words) {
    disc_read((uint64_t)offset_words << 2, addr, len);
    g_dvd_reads++; g_dvd_bytes += len;
}

/* the completion, in the interrupt-like frame: state, transferred size, then the callback */
static void dvd_done(ppc_context *ctx, uint8_t *base) {
    uint32_t blk = ctx->r[3];
    int i = pending_find(blk); if (i >= 0) pending[i] = pending[--npending];
    int32_t result;
    if ((int32_t)R32(blk + CB_STATE) == ST_CANCELED) result = -3;
    else { W32(blk + CB_TRANSFERRED, R32(blk + CB_LENGTH)); W32(blk + CB_STATE, ST_END); result = (int32_t)R32(blk + CB_LENGTH); }
    uint32_t cb = R32(blk + CB_CALLBACK);
    if (cb) { ctx->r[3] = (uint32_t)result; ctx->r[4] = blk; ppc_call_indirect(ctx, base, cb); }
}

/* DVDReadAbsAsyncPrio(block, addr, length, offset in words, callback, prio) (fn_801CECC0):
   behind DVDReadAsyncPrio and DVDReadPrio */
void hle_fn_801CECC0(ppc_context *ctx, uint8_t *base) {
    (void)base;
    static int reg; if (!reg) { reg = 1; rt_register_native(DVD_DONE_FN, dvd_done); }
    uint32_t blk = ctx->r[3], addr = ctx->r[4], len = ctx->r[5], off = ctx->r[6], cb = ctx->r[7];
    W32(blk + CB_COMMAND, 1); W32(blk + CB_ADDR, addr); W32(blk + CB_LENGTH, len); W32(blk + CB_OFFSET, off);
    W32(blk + CB_TRANSFERRED, 0); W32(blk + CB_CALLBACK, cb);
    do_read(addr, len, off);
    W32(blk + CB_STATE, ST_BUSY);
    if (pending_find(blk) < 0 && npending < 64) pending[npending++] = blk;
    rt_post_callback(DVD_DONE_FN, blk, 0);
    LOG("dvd", "read %08X <- %X bytes at %llX (async, cb %08X)", addr, len, (unsigned long long)off << 2, cb);
    ctx->r[3] = 1;
}

/* DVDReadPrio(fileInfo, addr, length, offset, prio): synchronous - copy and return the length */
void hle__Z11DVDReadPrioP9DVDHandlePviii(ppc_context *ctx, uint8_t *base) {
    (void)base;
    uint32_t fi = ctx->r[3], addr = ctx->r[4], len = ctx->r[5]; int32_t off = (int32_t)ctx->r[6];
    uint32_t flen = R32(fi + 0x34);
    if (off < 0 || (uint32_t)off > flen || (uint32_t)off + len >= flen + 0x20) {
        rt_log("dvd", "DVDReadPrio out of the file: offset %X length %X (file %X)", off, len, flen);
        ctx->r[3] = (uint32_t)-1; return;
    }
    uint32_t words = R32(fi + 0x30) + ((uint32_t)off >> 2);
    W32(fi + CB_COMMAND, 1); W32(fi + CB_ADDR, addr); W32(fi + CB_LENGTH, len); W32(fi + CB_OFFSET, words);
    do_read(addr, len, words);
    W32(fi + CB_TRANSFERRED, len); W32(fi + CB_STATE, ST_END);
    LOG("dvd", "read %08X <- %X bytes at %llX (sync)", addr, len, (unsigned long long)words << 2);
    ctx->r[3] = len;
}

/* DVDCancel(block): a read still waiting for its completion is canceled (its callback gets -3) */
void hle_DVDCancel(ppc_context *ctx, uint8_t *base) {
    uint32_t blk = ctx->r[3];
    if (pending_find(blk) < 0) { orig_DVDCancel(ctx, base); return; }   /* (not one of ours: the library's way) */
    if ((int32_t)R32(blk + CB_STATE) == ST_BUSY) W32(blk + CB_STATE, ST_CANCELED);
    ctx->r[3] = 0;
}
