/* rt_ios.c - Hollywood IPC registers + a small IOS (Starlet) emulation:
 * /dev/di (disc), /dev/fs + NAND files (host folder nand/), /dev/es, /dev/stm.
 * The IPC handshake follows Dolphin's WiiIPC/EmulationKernel exactly. */
#include "rt.h"
#include "rt_bt.h"
#include <sys/stat.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#include <unistd.h>
#define RMDIR(p) rmdir(p)
#endif

enum { IPC_OPEN = 1, IPC_CLOSE, IPC_READ, IPC_WRITE, IPC_SEEK, IPC_IOCTL, IPC_IOCTLV, IPC_REPLY };
enum { IOS_ENOENT = -6, IOS_EINVAL = -4, FS_ENOENT = -106, FS_EEXIST = -105 };
#define IRQ_IPC_BROADWAY 0x40000000u

/* ------------------------------------------------------------------ IPC registers */
static struct { uint8_t X1, X2, Y1, Y2, IX1, IX2, IY1, IY2; } c;
static uint32_t ppc_msg, arm_msg, irq_flags, irq_mask;
static uint32_t reqq[64], nreq, repq[64], nrep;
static uint8_t hwregs[0x400];

static void execute(uint32_t req);
static void update_irq(void) {
    if ((c.Y1 & c.IY1) || (c.Y2 & c.IY2)) irq_flags |= IRQ_IPC_BROADWAY;
    pi_set(PI_WII_IPC, (irq_flags & irq_mask) != 0);
}
static int ready(void) { return !c.Y1 && !c.Y2 && !(irq_flags & IRQ_IPC_BROADWAY); }
static void update_ipc(void) {
    if (!ready()) return;
    if (nreq) {
        c.X1 = 0; c.Y2 = 1;                      /* ack */
        uint32_t r = reqq[0]; memmove(reqq, reqq + 1, --nreq * 4);
        execute(r);
        return;
    }
    if (nrep) {
        arm_msg = repq[0]; memmove(repq, repq + 1, --nrep * 4);
        c.Y1 = 1;
    }
}
void ios_update(void) { bt_update(); update_ipc(); update_irq(); }

void ios_reply(uint32_t req, int32_t result);
static void reply(uint32_t req, int32_t result) { ios_reply(req, result); }
void ios_reply(uint32_t req, int32_t result) {
    uint32_t cmd = R32(req);
    W32(req + 4, (uint32_t)result);
    W32(req + 8, cmd);
    W32(req, IPC_REPLY);
    if (nrep < 64) repq[nrep++] = req;
}

uint32_t ios_mmio_read(uint32_t reg) {
    switch (reg) {
    case 0x04: return (c.IY2 << 5) | (c.IY1 << 4) | (c.X2 << 3) | (c.Y1 << 2) | (c.Y2 << 1) | c.X1;
    case 0x08: return arm_msg;
    case 0x30: return irq_flags;
    case 0x34: return irq_mask;
    case 0x64: return 0xFFFFFFFF;               /* AHBPROT */
    case 0xC8: return 0;                        /* GPIOB_IN */
    case 0x180: case 0x1CC: case 0x1D0: return 0;
    }
    return ((uint32_t)hwregs[reg] << 24) | (hwregs[reg + 1] << 16) | (hwregs[reg + 2] << 8) | hwregs[reg + 3];
}
void ios_mmio_write(uint32_t reg, uint32_t v) {
    switch (reg) {
    case 0x00: ppc_msg = v; return;
    case 0x04:
        c.X1 = v & 1; c.X2 = (v >> 3) & 1;
        if ((v >> 2) & 1) c.Y1 = 0;
        if ((v >> 1) & 1) c.Y2 = 0;
        c.IY1 = (v >> 4) & 1; c.IY2 = (v >> 5) & 1;
        if ((((v >> 2) & 1) && c.IY1) || (((v >> 1) & 1) && c.IY2)) irq_flags |= IRQ_IPC_BROADWAY;
        if (c.X1 && nreq < 64) reqq[nreq++] = ppc_msg;
        ios_update();
        return;
    case 0x30: irq_flags &= ~v; ios_update(); return;
    case 0x34:
        irq_mask = v;
        if (irq_mask & IRQ_IPC_BROADWAY) { memset(&c, 0, sizeof c); ppc_msg = arm_msg = 0; }
        ios_update();
        return;
    }
    hwregs[reg] = v >> 24; hwregs[reg + 1] = v >> 16; hwregs[reg + 2] = v >> 8; hwregs[reg + 3] = v;
}

/* ------------------------------------------------------------------ devices */
enum { DEV_NONE, DEV_DI, DEV_FS, DEV_ES, DEV_STM_IMM, DEV_STM_EVENT, DEV_FILE, DEV_STUB, DEV_BT };
typedef struct { int type; FILE *fp; char path[600]; uint32_t pos; int mode; } fdent;
static fdent fds[32];

static void nand_host_path(const char *nand, char *out, size_t n) {
    snprintf(out, n, "%s/nand%s", g_root, nand);
}
static void mkdirs_for(const char *path) {
    char tmp[700]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; MKDIR(tmp); *p = '/'; }
}
static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && (st.st_mode & S_IFDIR); }
static void remove_tree(const char *p) {                 /* FS Delete removes directories recursively */
    if (is_dir(p)) {
        DIR *d = opendir(p); struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char c[900]; snprintf(c, sizeof c, "%s/%s", p, de->d_name); remove_tree(c);
        }
        if (d) closedir(d);
        RMDIR(p);
    } else remove(p);
}
/* list a NAND directory: names separated by NULs, returns count (host junk files skipped) */
static int list_dir(const char *hp, char *names, size_t cap, int max) {
    DIR *d = opendir(hp); struct dirent *de; int n = 0; size_t o = 0;
    while (d && (de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.') continue;                  /* ., .., .fuse_hidden* */
        size_t l = strlen(de->d_name); if (l > 12) continue; /* NAND names are at most 12 chars */
        if (names) { if (o + l + 1 > cap) break; memcpy(names + o, de->d_name, l + 1); o += l + 1; }
        n++;
    }
    if (d) closedir(d);
    return n;
}

/* release host handles on a NAND path (and anything under it) before deleting/replacing it:
   Windows can't delete open files, and other hosts leave hidden placeholder files behind */
static void release_handles(const char *nand) {
    size_t n = strlen(nand);
    for (int i = 0; i < 32; i++)
        if (fds[i].type == DEV_FILE && fds[i].fp && !strncmp(fds[i].path, nand, n) && (fds[i].path[n] == 0 || fds[i].path[n] == '/')) {
            fclose(fds[i].fp); fds[i].fp = NULL;
        }
}
static int32_t dev_open(const char *path, int mode) {
    int fd = -1;
    for (int i = 0; i < 32; i++) if (!fds[i].type) { fd = i; break; }
    if (fd < 0) return -22;
    fdent *e = &fds[fd]; memset(e, 0, sizeof *e); e->mode = mode;
    snprintf(e->path, sizeof e->path, "%s", path);
    if (!strcmp(path, "/dev/di")) e->type = DEV_DI;
    else if (!strcmp(path, "/dev/fs")) e->type = DEV_FS;
    else if (!strcmp(path, "/dev/es")) e->type = DEV_ES;
    else if (!strcmp(path, "/dev/stm/immediate")) e->type = DEV_STM_IMM;
    else if (!strcmp(path, "/dev/stm/eventhook")) e->type = DEV_STM_EVENT;
    else if (!strcmp(path, "/dev/usb/oh1/57e/305")) e->type = DEV_BT;
    else if (!strncmp(path, "/dev/", 5)) {
        rt_log("ios", "open %s -> not available", path);
        return IOS_ENOENT;
    } else {
        char hp[700]; nand_host_path(path, hp, sizeof hp);
        if (!file_exists(hp)) { LOG("ios", "open NAND %s -> ENOENT", path); return FS_ENOENT; }
        e->fp = fopen(hp, (mode & 2) ? "r+b" : "rb");
        if (!e->fp) return FS_ENOENT;
        e->type = DEV_FILE;
    }
    LOG("ios", "open %s mode %d -> fd %d", path, mode, fd);
    return fd;
}

static uint32_t di_last_error;
static int32_t di_ioctl(uint32_t cmd, uint32_t in, uint32_t in_len, uint32_t out, uint32_t out_len, uint32_t req, int *pending) {
    (void)in_len; (void)req; (void)pending;
    LOG("di", "ioctl %02X in=%08X(%X) out=%08X(%X) args %08X %08X %08X", cmd, in, in_len, out, out_len,
        in_len >= 8 ? R32(in + 4) : 0, in_len >= 12 ? R32(in + 8) : 0, in_len >= 16 ? R32(in + 12) : 0);
    switch (cmd) {
    case 0x70: memcpy(G(out), G(0x80000000), 0x20); return 1;               /* ReadDiskID */
    case 0x71: {                                                             /* Read */
        uint32_t len = R32(in + 4), pos = R32(in + 8);
        if (out_len < len) return 0x20;
        disc_read((uint64_t)pos << 2, out, len);
        return 1;
    }
    case 0x88: if (out_len >= 4) W32(out, 2); return 1;                     /* cover: disc inside */
    case 0x7A: case 0x95: case 0x96: if (out_len >= 4) W32(out, 0); return 1;
    case 0x8D: {                                                             /* UnencryptedRead */
        uint32_t len = R32(in + 4), pos = R32(in + 8);
        /* error-001 check: the SDK reads past the end of a single-layer disc and expects
           a genuine drive to fail with "LBA out of range" */
        if (pos >= 0x46090000u) { di_last_error = 0x00052100; return 2; }
        memset(G(out), 0, len);
        if (pos == 0) memcpy(G(out), G(0x80000000), len < 0x20 ? len : 0x20);
        return 1;
    }
    case 0x12: if (out_len >= 8) { W32(out, 0x00000002); W32(out + 4, 0x20060526); } if (out_len >= 12) W32(out + 8, 0x41000000); return 1;
    case 0xDA: memset(G(out), 0, 0x40); W8(out + 0x33, 1); return 1;   /* BCA: NSMBW wants 0x33 zeros then 1 */
    case 0xA4: di_last_error = 0x00052000; return 2;   /* ReportKey: retail drives reject it */
    case 0xE0: if (out_len >= 4) W32(out, di_last_error); di_last_error = 0; return 1;   /* RequestError */
    case 0x79: *pending = 1; return 0;                                       /* WaitForCoverClose */
    default:
        LOG("ios", "di ioctl %02X (ok)", cmd);
        if (out && out_len) memset(G(out), 0, out_len);
        return 1;
    }
}

static int32_t fs_ioctl(uint32_t cmd, uint32_t in, uint32_t out, uint32_t out_len) {
    char hp[700];
    switch (cmd) {
    case 2:                                                                  /* GetStats */
        if (out_len >= 0x1C) { W32(out, 0x4000); W32(out + 4, 0x5DEC); W32(out + 8, 0x1DD4); W32(out + 12, 0x10);
                               W32(out + 16, 0x146B); W32(out + 20, 0x6C); W32(out + 24, 0x10); }
        return 0;
    case 3: case 9: {                                                        /* CreateDir / CreateFile */
        const char *p = (const char *)G(in + 6);
        nand_host_path(p, hp, sizeof hp); mkdirs_for(hp);
        if (file_exists(hp)) { LOG("ios", "fs create %s: exists", p); return FS_EEXIST; }
        if (cmd == 3) MKDIR(hp); else { FILE *f = fopen(hp, "wb"); if (f) fclose(f); }
        LOG("ios", "fs create %s %s", cmd == 3 ? "dir" : "file", p);
        return 0;
    }
    case 5: return 0;                                                        /* SetAttr */
    case 6: {                                                                /* GetAttr */
        const char *p = (const char *)G(in);
        nand_host_path(p, hp, sizeof hp);
        if (!file_exists(hp)) return FS_ENOENT;
        if (out_len >= 0x4C && is_dir(hp)) { memset(G(out), 0, 0x4C); W32(out, 0x1000); W16(out + 4, 1); W8(out + 0x46, 3); W8(out + 0x47, 3); W8(out + 0x48, 3); return 0; }
        if (out_len >= 0x4C) { memset(G(out), 0, 0x4C); W32(out, 0x1000); W16(out + 4, 1); memcpy(G(out + 6), p, strlen(p) < 63 ? strlen(p) : 63);
                               W8(out + 0x46, 3); W8(out + 0x47, 3); W8(out + 0x48, 3); }
        return 0;
    }
    case 7: {                                                                /* Delete */
        const char *p = (const char *)G(in); nand_host_path(p, hp, sizeof hp);
        if (!file_exists(hp)) return FS_ENOENT;
        release_handles(p); remove_tree(hp); LOG("ios", "fs delete %s", p); return 0;
    }
    case 8: {
        char hp2[700]; nand_host_path((const char *)G(in), hp, sizeof hp); nand_host_path((const char *)G(in + 0x40), hp2, sizeof hp2);
        if (!file_exists(hp)) return FS_ENOENT;
        release_handles((const char *)G(in)); release_handles((const char *)G(in + 0x40));
        mkdirs_for(hp2); remove_tree(hp2);
        LOG("ios", "fs rename %s -> %s", (const char *)G(in), (const char *)G(in + 0x40));
        return rename(hp, hp2) ? FS_ENOENT : 0;
    }
    }
    LOG("ios", "fs ioctl %u (ok) %s", cmd, (const char *)G(in));
    return 0;
}

static void execute(uint32_t req) {
    uint32_t cmd = R32(req); int32_t fd = (int32_t)R32(req + 8);
    int32_t res = IOS_EINVAL; int pending = 0;
    fdent *e = (fd >= 0 && fd < 32) ? &fds[fd] : NULL;
    switch (cmd) {
    case IPC_OPEN: res = dev_open((const char *)G(R32(req + 12)), (int)R32(req + 16)); break;
    case IPC_CLOSE:
        if (e && e->type) { if (e->fp) fclose(e->fp); memset(e, 0, sizeof *e); res = 0; }
        break;
    case IPC_READ: case IPC_WRITE: {
        uint32_t buf = R32(req + 12), len = R32(req + 16);
        if (e && e->type == DEV_FILE && !e->fp) res = -101;           /* file was deleted/replaced */
        else if (e && e->type == DEV_FILE) {
            fseek(e->fp, e->pos, SEEK_SET);
            size_t n = cmd == IPC_READ ? fread(G(buf), 1, len, e->fp) : fwrite(G(buf), 1, len, e->fp);
            LOG("ios", "%s %s pos %u len %u -> %u", cmd == IPC_READ ? "read" : "write", e->path, e->pos, len, (unsigned)n);
            e->pos += (uint32_t)n; res = (int32_t)n;
            if (cmd == IPC_WRITE) fflush(e->fp);
        } else res = IOS_EINVAL;
        break;
    }
    case IPC_SEEK:
        if (e && e->type == DEV_FILE && e->fp) {
            int32_t off = (int32_t)R32(req + 12); uint32_t whence = R32(req + 16);
            fseek(e->fp, 0, SEEK_END); long size = ftell(e->fp);
            int64_t np = whence == 0 ? off : whence == 1 ? (int64_t)e->pos + off : size + off;
            if (np < 0 || np > size) res = -101; else { e->pos = (uint32_t)np; res = (int32_t)np; }
        }
        break;
    case IPC_IOCTL: {
        uint32_t ioc = R32(req + 12), in = R32(req + 16), in_len = R32(req + 20), out = R32(req + 24), out_len = R32(req + 28);
        if (!e || !e->type) break;
        if (e->type == DEV_DI) res = di_ioctl(ioc, in, in_len, out, out_len, req, &pending);
        else if (e->type == DEV_FS) res = fs_ioctl(ioc, in, out, out_len);
        else if (e->type == DEV_STM_EVENT) pending = 1;                     /* power/reset events: never */
        else if (e->type == DEV_FILE && ioc == 11 && e->fp) {                        /* GetFileStats */
            fseek(e->fp, 0, SEEK_END); W32(out, (uint32_t)ftell(e->fp)); W32(out + 4, e->pos); res = 0;
        } else { LOG("ios", "ioctl %08X on %s (ok)", ioc, e->path); res = 0; }
        break;
    }
    case IPC_IOCTLV: {
        uint32_t ioc = R32(req + 12), nin = R32(req + 16), nout = R32(req + 20), vec = R32(req + 24);
        if (!e || !e->type) break;
        res = 0;
        if (e->type == DEV_FS) LOG("ios", "fs ioctlv %u nin %u nout %u path %s", ioc, nin, nout, nin ? (const char *)G(R32(vec)) : "-");
        if (e->type == DEV_BT) res = bt_ioctlv(req, ioc, nin, nout, vec, &pending);
        else if (e->type == DEV_ES) {
            if (ioc == 0x20 && nout >= 1) { uint32_t o = R32(vec + 8 * nin); W32(o, 0x00010000); W32(o + 4, 0x534D4E45); }  /* GetTitleID */
            else if (ioc == 0x1D && nin >= 1 && nout >= 1) {        /* GetTitleDir: the save folder ("home dir") */
                uint32_t ti = R32(vec), o = R32(vec + 8 * nin);
                char dir[64]; snprintf(dir, sizeof dir, "/title/%08x/%08x/data", R32(ti), R32(ti + 4));
                memcpy(G(o), dir, strlen(dir) + 1);
                char hp[700]; nand_host_path(dir, hp, sizeof hp); mkdirs_for(hp); MKDIR(hp);
                LOG("ios", "es title dir %s", dir);
            }
            else LOG("ios", "es ioctlv %02X (ok)", ioc);
        } else if (e->type == DEV_DI) {
            LOG("di", "ioctlv %02X nin=%u nout=%u", ioc, nin, nout);
            if (ioc == 0x8B) { res = 1; if (nout >= 2) W32(R32(vec + 8 * (nin + 1)), 0); }   /* OpenPartition */
            else res = 1;
        } else if (e->type == DEV_FS && ioc == 4) {                        /* ReadDir */
            char hp[700]; nand_host_path((const char *)G(R32(vec)), hp, sizeof hp);
            if (!file_exists(hp)) res = FS_ENOENT;
            else if (!is_dir(hp)) res = -101;                         /* a file: NANDGetType relies on this */
            else if (nin == 1 && nout >= 1) W32(R32(vec + 8), (uint32_t)list_dir(hp, NULL, 0, 1 << 20));   /* count only */
            else if (nin >= 2 && nout >= 2) {
                int max = (int)R32(R32(vec + 8)); uint32_t nb = R32(vec + 8 * nin), nl = R32(vec + 8 * nin + 4);
                W32(R32(vec + 8 * (nin + 1)), (uint32_t)list_dir(hp, (char *)G(nb), nl, max));
            }
        } else if (e->type == DEV_FS && ioc == 12) {                       /* GetUsage */
            if (nout >= 2) { W32(R32(vec + 8 * nin), 0); W32(R32(vec + 8 * (nin + 1)), 0); }
        } else LOG("ios", "ioctlv %08X on %s (ok)", ioc, e->path);
        break;
    }
    default: rt_log("ios", "unknown IPC command %u", cmd);
    }
    if (cmd != IPC_IOCTLV || !e || e->type != DEV_BT)
        LOG("ipc", "req %08X cmd %u fd %d (%s) -> %d%s", req, cmd, fd, e && e->type ? e->path : "-", res, pending ? " (pending)" : "");
    if (!pending) reply(req, res);
}

void ios_init(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/nand/shared2/sys/SYSCONF", g_root);
    if (!file_exists(p)) rt_log("ios", "note: no SYSCONF at %s (settings will use defaults)", p);
}
