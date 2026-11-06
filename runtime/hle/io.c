/* IoFileMgrForUser + StdioForUser.
 *
 *   disc0:/PATH, umd0:/PATH   ->  file inside the ISO ($MKU_ISO), found via ISO9660
 *   disc0:/sce_lbn0xL_size0xS ->  S bytes starting at sector L of the ISO
 *   umd0: / umd1: (no path)   ->  the whole disc as a block device: offsets and
 *                                 lengths are in 2048-byte sectors
 *   ms0:/PATH                 ->  $MKU_MS/PATH on the host (memory stick: saves)
 *
 * Games know where their files sit on the disc: sceIoGetstat reports a file's
 * first sector (LBN) in st_private[0], and many games then read the raw UMD
 * device directly. So we serve everything from the ISO image itself.
 */
#include "kernel.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILES 64
#define FD_BASE 3
#define SECTOR 2048u

enum FileKind { F_HOST, F_ISO, F_UMD_BLOCK };

typedef struct {
    int used;
    enum FileKind kind;
    int host_fd;            /* F_HOST */
    uint64_t base, size;    /* F_ISO: byte range inside the ISO */
    uint64_t pos;           /* F_ISO: bytes, F_UMD_BLOCK: sectors */
    char path[256];
    int64_t async_result;
} File;

static File g_files[MAX_FILES];
static int g_iso = -1;

static const char *ms_root(void) { const char *d = getenv("MKU_MS"); return d ? d : "build/ms0"; }

/* ------------------------------------------------------------------------ */
/* ISO9660: just enough to look up a path */

static int iso_fd(void) {
    if (g_iso < 0) {
        const char *path = getenv("MKU_ISO");
        if (!path || (g_iso = open(path, O_RDONLY)) < 0) {
            fprintf(stderr, "[io] set MKU_ISO to your game's ISO file (%s)\n", path ? strerror(errno) : "unset");
            exit(1);
        }
    }
    return g_iso;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Directory records: [0] length, [2] extent LBN, [10] data length, [25] flags, [32] name length, [33] name */
static int iso_lookup(const char *path, uint32_t *lbn, uint32_t *size, int *is_dir) {
    uint8_t pvd[SECTOR];
    if (pread(iso_fd(), pvd, SECTOR, 16 * SECTOR) != SECTOR) return 0;
    uint32_t dir_lbn = le32(pvd + 156 + 2), dir_size = le32(pvd + 156 + 10);
    *is_dir = 1;

    while (*path == '/') path++;
    while (*path) {
        const char *end = strchr(path, '/');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        uint8_t *dir = malloc(dir_size);
        if (pread(iso_fd(), dir, dir_size, (off_t)dir_lbn * SECTOR) != (ssize_t)dir_size) { free(dir); return 0; }
        int found = 0;
        for (uint32_t off = 0; off < dir_size;) {
            uint8_t *rec = dir + off;
            if (rec[0] == 0) { off = (off / SECTOR + 1) * SECTOR; continue; }   /* records never cross sectors */
            size_t nlen = rec[32];
            const char *name = (const char *)rec + 33;
            if (nlen >= 2 && name[nlen - 2] == ';') nlen -= 2;   /* "FILE.BIN;1" */
            if (nlen == len && !strncasecmp(name, path, len)) {
                dir_lbn = le32(rec + 2);
                dir_size = le32(rec + 10);
                *is_dir = (rec[25] & 2) != 0;
                found = 1;
                break;
            }
            off += rec[0];
        }
        free(dir);
        if (!found) return 0;
        path += len;
        while (*path == '/') path++;
    }
    *lbn = dir_lbn;
    *size = dir_size;
    return 1;
}

/* ------------------------------------------------------------------------ */

static int is_disc(const char *p) {
    /* PSP device names ignore case: the game itself asks for "MS0:/PSP/..." */
    return !strncasecmp(p, "disc0:", 6) || !strncasecmp(p, "umd0:", 5) || !strncasecmp(p, "umd1:", 5) || !strchr(p, ':');
}

static const char *after_device(const char *p) {
    const char *colon = strchr(p, ':');
    p = colon ? colon + 1 : p;
    while (*p == '/') p++;
    return p;
}

static int ms_path(const char *guest, char *out, size_t n) {
    if (strncasecmp(guest, "ms0:", 4) && strncasecmp(guest, "fatms0:", 7)) return 0;
    snprintf(out, n, "%s/%s", ms_root(), after_device(guest));
    return 1;
}

static File *file_get(uint32_t fd) {
    if (fd < FD_BASE || fd >= FD_BASE + MAX_FILES || !g_files[fd - FD_BASE].used) return NULL;
    return &g_files[fd - FD_BASE];
}

static int file_new(File f, const char *guest) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i] = f;
            g_files[i].used = 1;
            snprintf(g_files[i].path, sizeof g_files[i].path, "%s", guest);
            return FD_BASE + i;
        }
    }
    return (int)ERR_NO_MEMORY;
}

static void mkdirs_for(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}

static int do_open(const char *guest, uint32_t psp_flags) {
    if (is_disc(guest)) {
        const char *rest = after_device(guest);
        File f = {0};
        unsigned lbn_x = 0, size_x = 0;
        if (!*rest) {
            f.kind = F_UMD_BLOCK;
        } else if (sscanf(rest, "sce_lbn0x%x_size0x%x", &lbn_x, &size_x) == 2) {
            f.kind = F_ISO;
            f.base = (uint64_t)lbn_x * SECTOR;
            f.size = size_x;
        } else {
            uint32_t lbn, size;
            int dir;
            char full[256];
            snprintf(full, sizeof full, "%s%s", strchr(guest, ':') ? "" : "PSP_GAME/USRDIR/", rest);
            if (!iso_lookup(full, &lbn, &size, &dir) || dir) {
                fprintf(stderr, "[io] not on disc: %s\n", guest);
                return (int)ERR_FILE_NOT_FOUND;
            }
            f.kind = F_ISO;
            f.base = (uint64_t)lbn * SECTOR;
            f.size = size;
        }
        iso_fd();
        return file_new(f, guest);
    }

    char host[256];
    if (!ms_path(guest, host, sizeof host)) {
        fprintf(stderr, "[io] unknown device: %s\n", guest);
        return (int)ERR_FILE_NOT_FOUND;
    }
    int flags = (psp_flags & 3) == 3 ? O_RDWR : (psp_flags & 2) ? O_WRONLY : O_RDONLY;
    if (psp_flags & 0x100) flags |= O_APPEND;
    if (psp_flags & 0x200) flags |= O_CREAT;
    if (psp_flags & 0x400) flags |= O_TRUNC;
    if (psp_flags & 0x800) flags |= O_EXCL;
    if (flags & O_CREAT) mkdirs_for(host);
    int hfd = open(host, flags, 0644);
    if (hfd < 0) return (int)ERR_FILE_NOT_FOUND;
    return file_new((File){.kind = F_HOST, .host_fd = hfd}, guest);
}

void hle_sceIoOpen(Ctx *c) {
    int fd = do_open(GSTR(A0), A1);
    TRACE("(\"%s\", 0x%X) -> 0x%X", GSTR(A0), A1, fd);
    RET(fd);
}

void hle_sceIoOpenAsync(Ctx *c) {
    int fd = do_open(GSTR(A0), A1);
    TRACE("(\"%s\", 0x%X) -> 0x%X", GSTR(A0), A1, fd);
    File *f = file_get((uint32_t)fd);
    if (f) f->async_result = fd;
    RET(fd);
}

void hle_sceIoClose(Ctx *c) {
    File *f = file_get(A0);
    if (!f) RET(0x80020323u);   /* bad file descriptor */
    TRACE("(0x%X \"%s\")", A0, f->path);
    if (f->kind == F_HOST) close(f->host_fd);
    f->used = 0;
    RET(0);
}

static int64_t do_read(File *f, uint32_t buf, uint32_t len) {
    ssize_t n;
    switch (f->kind) {
    case F_HOST:
        n = read(f->host_fd, rt_ptr(buf), len);
        return n < 0 ? (int64_t)0x80010005 : n;
    case F_ISO:
        if (f->pos >= f->size) return 0;
        if (len > f->size - f->pos) len = (uint32_t)(f->size - f->pos);
        n = pread(iso_fd(), rt_ptr(buf), len, (off_t)(f->base + f->pos));
        if (n > 0) f->pos += (uint64_t)n;
        return n < 0 ? (int64_t)0x80010005 : n;
    case F_UMD_BLOCK:   /* len and the result are sector counts */
        n = pread(iso_fd(), rt_ptr(buf), (size_t)len * SECTOR, (off_t)(f->pos * SECTOR));
        if (n < 0) return (int64_t)0x80010005;
        f->pos += (uint64_t)n / SECTOR;
        return n / SECTOR;
    }
    return 0;
}

void hle_sceIoRead(Ctx *c) {
    File *f = file_get(A0);
    if (!f) RET(0x80020323u);
    uint64_t pos = f->pos;
    int64_t n = do_read(f, A1, A2);
    TRACE("(0x%X \"%s\" @%llu, %u) -> %lld", A0, f->path, (unsigned long long)pos, A2, (long long)n);
    RET(n);
}

void hle_sceIoReadAsync(Ctx *c) {
    File *f = file_get(A0);
    if (!f) RET(0x80020323u);
    uint64_t pos = f->pos;
    f->async_result = do_read(f, A1, A2);   /* done immediately; the game collects it with WaitAsync */
    TRACE("(0x%X \"%s\" @%llu, %u) -> %lld", A0, f->path, (unsigned long long)pos, A2, (long long)f->async_result);
    RET(0);
}

void hle_sceIoWrite(Ctx *c) {
    uint32_t fd = A0, buf = A1, size = A2;
    if (fd == 1 || fd == 2) {   /* the game's debug output */
        fprintf(stderr, "[game] %.*s", (int)size, (const char *)rt_ptr(buf));
        RET(size);
    }
    File *f = file_get(fd);
    if (!f || f->kind != F_HOST) RET(0x80020323u);
    ssize_t n = write(f->host_fd, rt_ptr(buf), size);
    RET(n < 0 ? 0x80010005u : (uint32_t)n);
}

void hle_sceIoLseek(Ctx *c) {
    File *f = file_get(A0);
    if (!f) RET64(-1);
    int64_t off = (int64_t)(((uint64_t)A3 << 32) | A2);   /* 64-bit argument in a2:a3 */
    uint32_t whence = T0;
    TRACE("(0x%X \"%s\", %lld, %u)", A0, f->path, (long long)off, whence);
    if (f->kind == F_HOST) RET64(lseek(f->host_fd, off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET));
    uint64_t end = f->kind == F_ISO ? f->size : (uint64_t)lseek(iso_fd(), 0, SEEK_END) / SECTOR;
    int64_t base = whence == 1 ? (int64_t)f->pos : whence == 2 ? (int64_t)end : 0;
    f->pos = (uint64_t)(base + off);
    RET64(f->pos);
}

static void write_async_result(File *f, uint32_t addr) {
    if (!addr) return;
    MEM_SW(addr, (uint32_t)f->async_result);
    MEM_SW(addr + 4, (uint32_t)((uint64_t)f->async_result >> 32));
}

void hle_sceIoWaitAsync(Ctx *c) {
    File *f = file_get(A0);
    if (!f) RET(0x80020323u);
    write_async_result(f, A1);
    RET(0);
}
void hle_sceIoWaitAsyncCB(Ctx *c) { kernel_run_callbacks(c); hle_sceIoWaitAsync(c); }
void hle_sceIoPollAsync(Ctx *c) { hle_sceIoWaitAsync(c); }
void hle_sceIoChangeAsyncPriority(Ctx *c) { RET(0); }

void hle_sceIoGetstat(Ctx *c) {
    const char *guest = GSTR(A0);
    uint32_t s = A1;
    memset(rt_ptr(s), 0, 88);
    /* SceIoStat: mode, attr, size (64-bit), ctime/atime/mtime (16 bytes each), private[6] at 0x40 */
    if (is_disc(guest)) {
        uint32_t lbn, size;
        int dir;
        char full[256];
        snprintf(full, sizeof full, "%s%s", strchr(guest, ':') ? "" : "PSP_GAME/USRDIR/", after_device(guest));
        if (!iso_lookup(full, &lbn, &size, &dir)) RET(ERR_FILE_NOT_FOUND);
        MEM_SW(s, dir ? 0x116D : 0x216D);
        MEM_SW(s + 4, dir ? 0x10 : 0x20);
        MEM_SW(s + 8, size);
        MEM_SW(s + 0x40, lbn);   /* the file's first sector on the disc */
        TRACE("(\"%s\") -> size %u, lbn 0x%X", guest, size, lbn);
        RET(0);
    }
    char host[256];
    struct stat st;
    if (!ms_path(guest, host, sizeof host) || stat(host, &st) != 0) RET(ERR_FILE_NOT_FOUND);
    int dir = S_ISDIR(st.st_mode);
    MEM_SW(s, dir ? 0x11FF : 0x21FF);
    MEM_SW(s + 4, dir ? 0x10 : 0x20);
    MEM_SW(s + 8, (uint32_t)st.st_size);
    MEM_SW(s + 12, (uint32_t)((uint64_t)st.st_size >> 32));
    RET(0);
}

void hle_sceIoMkdir(Ctx *c) {
    char host[256];
    if (!ms_path(GSTR(A0), host, sizeof host)) RET(ERR_FILE_NOT_FOUND);
    mkdirs_for(host);
    RET(mkdir(host, 0755) == 0 || errno == EEXIST ? 0 : 0x80010011u);
}

void hle_sceIoRmdir(Ctx *c) {
    char host[256];
    if (!ms_path(GSTR(A0), host, sizeof host)) RET(ERR_FILE_NOT_FOUND);
    RET(rmdir(host) == 0 ? 0 : ERR_FILE_NOT_FOUND);
}

void hle_sceIoRemove(Ctx *c) {
    char host[256];
    if (!ms_path(GSTR(A0), host, sizeof host)) RET(ERR_FILE_NOT_FOUND);
    RET(unlink(host) == 0 ? 0 : ERR_FILE_NOT_FOUND);
}

void hle_sceIoRename(Ctx *c) {
    char from[256], to[256];
    if (!ms_path(GSTR(A0), from, sizeof from) || !ms_path(GSTR(A1), to, sizeof to)) RET(ERR_FILE_NOT_FOUND);
    RET(rename(from, to) == 0 ? 0 : ERR_FILE_NOT_FOUND);
}

/* Device control. Only the memory stick queries games make at boot: a memory
 * stick is always inserted, writable and has plenty of free space. */
void hle_sceIoDevctl(Ctx *c) {
    const char *dev = GSTR(A0);
    uint32_t cmd = A1, in = A2, in_len = A3, out = T0, out_len = T1;
    TRACE("(\"%s\", cmd=0x%08X)", dev, cmd);
    switch (cmd) {
    case 0x02415821:   /* fatms0: register insert/eject callback -> report "inserted" now */
    case 0x02015804:   /* mscmhc0: same */
        if (in && in_len >= 4) kernel_notify_callback((int)MEM_LW(in), 1);
        RET(0);
    case 0x02415822: case 0x02015805:   /* unregister */
        RET(0);
    case 0x02425823:   /* is the FAT file system available? 1 = yes */
    case 0x02025806:   /* is a memory stick inserted? 1 = yes */
        if (out) MEM_SW(out, 1);
        RET(0);
    case 0x02025801:   /* driver state: 4 = device inserted */
        if (out && out_len >= 4) MEM_SW(out, 4);
        RET(0);
    case 0x02425824:   /* write protected? 0 = no */
        if (out) MEM_SW(out, 0);
        RET(0);
    case 0x02425818: {   /* capacity: in = pointer to a pointer to {maxClusters, freeClusters, maxSectors, sectorSize, sectorCount} */
        uint32_t p = in ? MEM_LW(in) : 0;
        if (p) {
            uint32_t clusters = (uint32_t)((1ull << 30) / (0x200 * 64));   /* 1 GiB free, 32 KiB clusters */
            MEM_SW(p, clusters); MEM_SW(p + 4, clusters); MEM_SW(p + 8, clusters);
            MEM_SW(p + 12, 0x200); MEM_SW(p + 16, 64);
        }
        RET(0);
    }
    }
    LOG("(\"%s\", cmd=0x%08X) -> 0 (not implemented)", dev, cmd);
    (void)out_len;
    RET(0);
}

/* StdioForUser */
void hle_sceKernelStdin(Ctx *c) { RET(0); }
void hle_sceKernelStdout(Ctx *c) { RET(1); }
void hle_sceKernelStderr(Ctx *c) { RET(2); }
