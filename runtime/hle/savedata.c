/*
 * sceUtilitySavedata*: the PSP's save-data system dialog.
 *
 * Games never write saves themselves: they fill a SceUtilitySavedataParam
 * and run the system utility, polling its status:
 *     InitStart -> status 1 (init) -> 2 (running) -> Update... -> 3 (finished)
 *     -> ShutdownStart -> 4 (shutdown) -> 0 (none)
 * We do the work immediately (no dialog on screen yet) and walk the same
 * status sequence. Saves are real files under $MKU_MS/PSP/SAVEDATA/<game><save>/.
 * Layout and error codes follow PPSSPP's Core/Dialog/SavedataParam.h.
 */
#include "kernel.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* SceUtilitySavedataParam offsets */
enum {
    SD_RESULT = 0x1C, SD_MODE = 0x30, SD_GAMENAME = 0x3C, SD_SAVENAME = 0x4C, SD_SAVENAMELIST = 0x60,
    SD_FILENAME = 0x64, SD_DATABUF = 0x74, SD_DATABUFSIZE = 0x78, SD_DATASIZE = 0x7C, SD_SFO = 0x80,
    SD_ICON0 = 0x584, SD_ICON1 = 0x594, SD_PIC1 = 0x5A4, SD_SND0 = 0x5B4,
    SD_MSFREE = 0x5D0, SD_MSDATA = 0x5D4, SD_UTILDATA = 0x5D8,
};

#define ERR_LOAD_NO_DATA  0x80110307u
#define ERR_SIZES_NO_DATA 0x801103C7u
#define ERR_DELETE_NO_DATA 0x80110347u

enum { ST_NONE = 0, ST_INIT = 1, ST_RUNNING = 2, ST_FINISHED = 3, ST_SHUTDOWN = 4 };
static int g_status;
static uint32_t g_param;

static const char *ms_root(void) { const char *d = getenv("MKU_MS"); return d ? d : "build/ms0"; }

static void gstr(uint32_t addr, size_t max, char *out) {
    size_t i;
    for (i = 0; i + 1 < max && MEM_LBU(addr + i); i++) out[i] = (char)MEM_LBU(addr + i);
    out[i] = 0;
}

static void save_dir(uint32_t p, const char *save_name, char *out, size_t n) {
    char game[16];
    gstr(p + SD_GAMENAME, 13, game);
    snprintf(out, n, "%s/PSP/SAVEDATA/%s%s", ms_root(), game, save_name);
}

static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
    mkdir(tmp, 0755);
}

static int dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static long dir_bytes(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    long total = 0;
    struct dirent *e;
    char f[600];
    struct stat st;
    while ((e = readdir(d))) {
        snprintf(f, sizeof f, "%s/%s", path, e->d_name);
        if (stat(f, &st) == 0 && S_ISREG(st.st_mode)) total += st.st_size;
    }
    closedir(d);
    return total;
}

/* For the LIST* modes: the first name in saveNameList with/without data. */
static int pick_from_list(uint32_t p, int want_existing, char *name) {
    uint32_t list = MEM_LW(p + SD_SAVENAMELIST);
    char dir[512];
    for (int i = 0; list && i < 100; i++) {
        gstr(list + 20u * (uint32_t)i, 20, name);
        if (!name[0]) break;
        save_dir(p, name, dir, sizeof dir);
        if (dir_exists(dir) == want_existing) return 1;
    }
    gstr(p + SD_SAVENAME, 20, name);
    return name[0] || list == 0;
}

static void write_file(const char *dir, const char *file, uint32_t buf, uint32_t size) {
    if (!buf || !size) return;
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(rt_ptr(buf), 1, size, f);
    fclose(f);
}

static void fill_used(uint32_t info, long bytes) {   /* SceUtilitySavedataUsedDataInfo */
    uint32_t clusters = (uint32_t)((bytes + 32767) / 32768);
    MEM_SW(info, clusters);
    MEM_SW(info + 4, clusters * 32);
    snprintf((char *)rt_ptr(info + 8), 8, "%u KB", clusters * 32);
    MEM_SW(info + 16, clusters * 32);
    snprintf((char *)rt_ptr(info + 20), 8, "%u KB", clusters * 32);
}

static uint32_t run_operation(uint32_t p) {
    int mode = (int)MEM_LW(p + SD_MODE);
    char name[24], dir[512], file[16], path[600];
    gstr(p + SD_SAVENAME, 20, name);
    gstr(p + SD_FILENAME, 13, file);
    save_dir(p, name, dir, sizeof dir);
    LOG("(mode %d, save \"%s\", file \"%s\") -> %s", mode, name, file, dir);

    switch (mode) {
    case 0: case 2: case 4: case 15: case 16: {   /* (auto/list) load, read data */
        if (mode == 4 && !pick_from_list(p, 1, name)) return ERR_LOAD_NO_DATA;
        save_dir(p, name, dir, sizeof dir);
        snprintf(path, sizeof path, "%s/%s", dir, file);
        FILE *f = fopen(path, "rb");
        if (!f) return ERR_LOAD_NO_DATA;
        size_t n = fread(rt_ptr(MEM_LW(p + SD_DATABUF)), 1, MEM_LW(p + SD_DATABUFSIZE), f);
        fclose(f);
        MEM_SW(p + SD_DATASIZE, (uint32_t)n);
        if (mode == 4) memcpy(rt_ptr(p + SD_SAVENAME), name, strlen(name) + 1);
        return 0;
    }
    case 1: case 3: case 5: case 13: case 14: case 17: case 18: {   /* (auto/list) save, make, write */
        if (mode == 5) {
            if (!pick_from_list(p, 1, name)) pick_from_list(p, 0, name);
            save_dir(p, name, dir, sizeof dir);
            memcpy(rt_ptr(p + SD_SAVENAME), name, strlen(name) + 1);
        }
        mkdirs(dir);
        write_file(dir, file, MEM_LW(p + SD_DATABUF), MEM_LW(p + SD_DATASIZE));
        write_file(dir, "ICON0.PNG", MEM_LW(p + SD_ICON0), MEM_LW(p + SD_ICON0 + 8));
        write_file(dir, "ICON1.PMF", MEM_LW(p + SD_ICON1), MEM_LW(p + SD_ICON1 + 8));
        write_file(dir, "PIC1.PNG", MEM_LW(p + SD_PIC1), MEM_LW(p + SD_PIC1 + 8));
        write_file(dir, "SND0.AT3", MEM_LW(p + SD_SND0), MEM_LW(p + SD_SND0 + 8));
        char title[0x81];
        gstr(p + SD_SFO + 0x80, 0x80, title);
        snprintf(path, sizeof path, "%s/TITLE.txt", dir);   /* stand-in for PARAM.SFO */
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%s\n", title); fclose(f); }
        return 0;
    }
    case 8: {   /* SIZES: free space, size of an existing save, size a new save needs */
        uint32_t free_info = MEM_LW(p + SD_MSFREE), data_info = MEM_LW(p + SD_MSDATA), util = MEM_LW(p + SD_UTILDATA);
        uint32_t result = 0;
        if (free_info) {   /* clusterSize, freeClusters, freeSpaceKB, freeSpaceStr */
            MEM_SW(free_info, 32768);
            MEM_SW(free_info + 4, 32768);
            MEM_SW(free_info + 8, 32768 * 32);
            snprintf((char *)rt_ptr(free_info + 12), 8, "1 GB");
        }
        if (data_info) {   /* gameName[13] pad[3] saveName[20] info */
            char g[16], s[24], d[512];
            gstr(data_info, 13, g);
            gstr(data_info + 16, 20, s);
            snprintf(d, sizeof d, "%s/PSP/SAVEDATA/%s%s", ms_root(), g, s);
            if (dir_exists(d)) fill_used(data_info + 36, dir_bytes(d));
            else { memset(rt_ptr(data_info + 36), 0, 28); result = ERR_SIZES_NO_DATA; }
        }
        if (util) {
            long need = (long)MEM_LW(p + SD_DATASIZE) + MEM_LW(p + SD_ICON0 + 8) + MEM_LW(p + SD_ICON1 + 8) +
                        MEM_LW(p + SD_PIC1 + 8) + MEM_LW(p + SD_SND0 + 8) + 0x1000;
            fill_used(util, need);
        }
        return result;
    }
    case 6: case 7: case 9: case 10: case 19: case 20: case 21: {   /* delete */
        if (!dir_exists(dir)) return ERR_DELETE_NO_DATA;
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            unlink(path);
        }
        if (d) closedir(d);
        rmdir(dir);
        return 0;
    }
    }
    LOG("(mode %d) not implemented -- reporting success", mode);
    return 0;
}

void hle_sceUtilitySavedataInitStart(Ctx *c) {
    g_param = A0;
    MEM_SW(g_param + SD_RESULT, run_operation(g_param));
    TRACE("(mode %u) -> result 0x%X", MEM_LW(g_param + SD_MODE), MEM_LW(g_param + SD_RESULT));
    g_status = ST_INIT;
    RET(0);
}

void hle_sceUtilitySavedataGetStatus(Ctx *c) {
    int s = g_status;
    if (s == ST_INIT) g_status = ST_RUNNING;
    else if (s == ST_RUNNING) g_status = ST_FINISHED;   /* the work is already done */
    else if (s == ST_SHUTDOWN) g_status = ST_NONE;
    RET(s);
}

void hle_sceUtilitySavedataUpdate(Ctx *c) {
    if (g_status == ST_RUNNING) g_status = ST_FINISHED;
    RET(0);
}

void hle_sceUtilitySavedataShutdownStart(Ctx *c) {
    g_status = ST_SHUTDOWN;
    RET(0);
}
