/* SysMemUserForUser (memory), Kernel_Library, UtilsForUser, sceSuspendForUser. */
#include "kernel.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

/* ------------------------------------------------------------------------ */
/* user memory allocator: 0x08800000-0x0A000000, first fit, 256-byte units */

#define USER_LO 0x08800000u
#define USER_HI 0x0A000000u
#define ROOT_RESERVED 0x10000u   /* top 64 KiB: root thread stack + argv (main_game.c) */
#define MAX_BLOCKS 1024

typedef struct { uint32_t addr, size; } Block;
static Block g_blocks[MAX_BLOCKS];   /* sorted by address */
static int g_nblocks;

static void block_insert(uint32_t addr, uint32_t size) {
    int i = g_nblocks++;
    while (i > 0 && g_blocks[i - 1].addr > addr) { g_blocks[i] = g_blocks[i - 1]; i--; }
    g_blocks[i] = (Block){addr, size};
}

static int block_fits(uint32_t start, uint32_t size) {
    if (start < USER_LO || start + size > USER_HI) return 0;
    for (int i = 0; i < g_nblocks; i++)
        if (start < g_blocks[i].addr + g_blocks[i].size && g_blocks[i].addr < start + size) return 0;
    return 1;
}

uint32_t mem_alloc(uint32_t size, int from_top, uint32_t align) {
    size = (size + 0xFF) & ~0xFFu;
    if (align < 256) align = 256;
    if (g_nblocks == MAX_BLOCKS) return 0;
    if (!from_top) {
        uint32_t cand = USER_LO;
        for (int i = 0; i <= g_nblocks; i++) {
            uint32_t start = (cand + align - 1) & ~(align - 1);
            uint32_t limit = i < g_nblocks ? g_blocks[i].addr : USER_HI;
            if (start + size <= limit && start + size > start) { block_insert(start, size); return start; }
            if (i < g_nblocks && g_blocks[i].addr + g_blocks[i].size > cand) cand = g_blocks[i].addr + g_blocks[i].size;
        }
    } else {
        uint32_t end = USER_HI;
        for (int i = g_nblocks - 1; i >= -1; i--) {
            uint32_t start = (end - size) & ~(align - 1);
            uint32_t floor = i >= 0 ? g_blocks[i].addr + g_blocks[i].size : USER_LO;
            if (end >= size && start >= floor) { block_insert(start, size); return start; }
            if (i >= 0 && g_blocks[i].addr < end) end = g_blocks[i].addr;
        }
    }
    return 0;
}

static uint32_t mem_alloc_at(uint32_t addr, uint32_t size) {
    size = (size + 0xFF) & ~0xFFu;
    if (!block_fits(addr, size)) return 0;
    block_insert(addr, size);
    return addr;
}

void mem_free(uint32_t addr) {
    for (int i = 0; i < g_nblocks; i++) {
        if (g_blocks[i].addr == addr) {
            memmove(&g_blocks[i], &g_blocks[i + 1], (size_t)(g_nblocks - i - 1) * sizeof(Block));
            g_nblocks--;
            return;
        }
    }
}

uint32_t mem_free_total(void) {
    uint32_t used = 0;
    for (int i = 0; i < g_nblocks; i++) used += g_blocks[i].size;
    return USER_HI - USER_LO - used;
}

uint32_t mem_free_max(void) {
    uint32_t best = 0, cand = USER_LO;
    for (int i = 0; i <= g_nblocks; i++) {
        uint32_t limit = i < g_nblocks ? g_blocks[i].addr : USER_HI;
        if (limit > cand && limit - cand > best) best = limit - cand;
        if (i < g_nblocks) cand = g_blocks[i].addr + g_blocks[i].size;
    }
    return best;
}

/* Called once at startup: the module image and the root stack are taken. */
void sysmem_init(void) {
    block_insert(USER_LO, (rt_image_base + rt_image_size - USER_LO + 0xFF) & ~0xFFu);
    block_insert(USER_HI - ROOT_RESERVED, ROOT_RESERVED);
}

/* ------------------------------------------------------------------------ */

typedef struct { uint32_t addr, size; char name[32]; } MemBlock;

void hle_sceKernelAllocPartitionMemory(Ctx *c) {
    uint32_t type = A2, size = A3, want = T0;
    uint32_t addr = type == 2 ? mem_alloc_at(want, size) : mem_alloc(size, type == 1, 256);
    TRACE("(part=%u, \"%s\", type=%u, size=0x%X) -> 0x%08X", A0, GSTR(A1), type, size, addr);
    if (!addr) RET(ERR_NO_MEMORY);
    MemBlock *b = calloc(1, sizeof *b);
    b->addr = addr;
    b->size = size;
    snprintf(b->name, sizeof b->name, "%s", GSTR(A1));
    RET(kobj_new(OBJ_MEMBLOCK, b));
}

void hle_sceKernelGetBlockHeadAddr(Ctx *c) {
    MemBlock *b = kobj_get((int)A0, OBJ_MEMBLOCK);
    RET(b ? b->addr : 0);
}

void hle_sceKernelFreePartitionMemory(Ctx *c) {
    MemBlock *b = kobj_get((int)A0, OBJ_MEMBLOCK);
    if (!b) RET(ERR_ILLEGAL_ARGUMENT);
    TRACE("(\"%s\" 0x%08X)", b->name, b->addr);
    mem_free(b->addr);
    kobj_free((int)A0);
    free(b);
    RET(0);
}

void hle_sceKernelMaxFreeMemSize(Ctx *c) { RET(mem_free_max()); }
void hle_sceKernelTotalFreeMemSize(Ctx *c) { RET(mem_free_total()); }
void hle_sceKernelSetCompiledSdkVersion(Ctx *c) { TRACE("(0x%08X)", A0); RET(0); }
void hle_sceKernelSetCompilerVersion(Ctx *c) { TRACE("(0x%08X)", A0); RET(0); }

/* printf with arguments in guest registers (a1.. then t0-t3). Integers and
 * strings only; floats would arrive as doubles and are printed as '?'. */
void hle_sceKernelPrintf(Ctx *c) {
    const char *f = GSTR(A0);
    uint32_t args[7] = {c->r[5], c->r[6], c->r[7], c->r[8], c->r[9], c->r[10], c->r[11]};
    int ai = 0;
    char out[1024];
    size_t o = 0;
    while (*f && o < sizeof out - 64) {
        if (*f != '%') { out[o++] = *f++; continue; }
        char spec[16];
        size_t s = 0;
        spec[s++] = *f++;
        while (*f && strchr("-+ #0123456789.lh", *f) && s < 12) {
            if (*f != 'l' && *f != 'h') spec[s++] = *f;   /* guest ints are 32-bit: drop size modifiers */
            f++;
        }
        char conv = *f ? *f++ : 0;
        uint32_t v = ai < 7 ? args[ai] : 0;
        if (conv == '%') { out[o++] = '%'; continue; }
        ai++;
        spec[s++] = conv == 'p' ? 'x' : conv;
        spec[s] = 0;
        if (conv == 's') { spec[s - 1] = 's'; o += (size_t)snprintf(out + o, sizeof out - o, spec, GSTR(v)); }
        else if (strchr("diuxXc", conv)) o += (size_t)snprintf(out + o, sizeof out - o, spec, v);
        else if (conv == 'p') o += (size_t)snprintf(out + o, sizeof out - o, "0x%08X", v);
        else out[o++] = '?';
    }
    out[o] = 0;
    fprintf(stderr, "[game] %s%s", out, o && out[o - 1] == '\n' ? "" : "\n");
    RET(0);
}

/* ------------------------------------------------------------------------ */
/* Kernel_Library: interrupts are never really disabled on our side. */
void hle_sceKernelCpuSuspendIntr(Ctx *c) { RET(1); }
void hle_sceKernelCpuResumeIntr(Ctx *c) { RET(0); }
void hle_sceKernelCpuResumeIntrWithSync(Ctx *c) { RET(0); }
void hle_sceKernelIsCpuIntrEnable(Ctx *c) { RET(1); }

/* UtilsForUser */
void hle_sceKernelDcacheWritebackAll(Ctx *c) { RET(0); }
void hle_sceKernelDcacheWritebackInvalidateAll(Ctx *c) { RET(0); }
void hle_sceKernelDcacheWritebackRange(Ctx *c) { RET(0); }
void hle_sceKernelSetGPO(Ctx *c) { RET(0); }
void hle_sceKernelGetGPI(Ctx *c) { RET(0); }
void hle_sceKernelLibcClock(Ctx *c) { RET(now_us()); }
void hle_sceKernelLibcTime(Ctx *c) {
    /* MKU_TIME=<unix time>: a fixed clock, so random choices (MK seeds its
     * RNG from the time) repeat between test runs */
    const char *fixed = getenv("MKU_TIME");
    uint32_t t = fixed ? (uint32_t)strtoul(fixed, NULL, 0) : (uint32_t)time(NULL);
    if (A0) MEM_SW(A0, t);
    RET(t);
}
void hle_sceKernelLibcGettimeofday(Ctx *c) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (A0) { MEM_SW(A0, (uint32_t)tv.tv_sec); MEM_SW(A0 + 4, (uint32_t)tv.tv_usec); }
    RET(0);
}

/* sceSuspendForUser. "Volatile memory" is the extra 4 MiB the PSP-2000 lends
 * to games at 0x08400000; our address space has it, so lend it out. */
void hle_sceKernelPowerLock(Ctx *c) { RET(0); }
void hle_sceKernelPowerUnlock(Ctx *c) { RET(0); }
void hle_sceKernelPowerTick(Ctx *c) { RET(0); }
void hle_sceKernelVolatileMemTryLock(Ctx *c) {
    TRACE("(%u)", A0);
    if (A1) MEM_SW(A1, 0x08400000u);
    if (A2) MEM_SW(A2, 0x00400000u);
    RET(0);
}
void hle_sceKernelVolatileMemUnlock(Ctx *c) { RET(0); }

/* sceDmac: the DMA engine; a plain memory copy for us (dst, src, size). */
void hle_sceDmacMemcpy(Ctx *c) {
    memmove(rt_ptr(A0), rt_ptr(A1), A2);
    RET(0);
}
