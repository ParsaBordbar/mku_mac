/* Runtime for recompiled code: memory, indirect calls, diagnostics. */
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

uint8_t *rt_mem;
volatile uint32_t rt_cur_fn;
static RtShadow g_boot_shadow;
RtShadow *rt_shadow = &g_boot_shadow;

void rt_thread_high_priority(void) {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

void rt_shadow_print(const RtShadow *s, const char *indent) {
    int n = s->depth < RT_SHADOW_MAX ? s->depth : RT_SHADOW_MAX;
    for (int i = n - 1; i >= 0 && i >= n - 12; i--)   /* innermost first */
        fprintf(stderr, "%sfunc_%08X\n", indent, s->fn[i] - rt_image_base);
}

void rt_init_memory(void) {
    /* 1 GiB of address space; macOS only commits pages that are touched. */
    rt_mem = mmap(NULL, RT_MEM_MASK + 1u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rt_mem == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
}

int rt_load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }
    size_t n = fread(rt_ptr(rt_image_base), 1, rt_image_size, f);
    fclose(f);
    if (n != rt_image_size) {
        fprintf(stderr, "%s: expected %u bytes, got %zu\n", path, rt_image_size, n);
        return -1;
    }
    return 0;
}

static const RecompFnEntry *find_function(uint32_t addr) {
    uint32_t lo = 0, hi = rt_func_table_len;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rt_func_table[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < rt_func_table_len && rt_func_table[lo].addr == addr) return &rt_func_table[lo];
    return NULL;
}

int rt_have_function(uint32_t addr) { return !rt_interp_only && find_function(addr) != NULL; }
int rt_interp_only;   /* tests: interpret everything */

void rt_call_indirect(Ctx *c, uint32_t addr) {
    const RecompFnEntry *e = rt_interp_only ? NULL : find_function(addr);
    /* Not a recompiled function: code loaded or generated at runtime (MK's
     * .mko script overlays), or an entry point the analysis missed. Run it
     * in the interpreter if the address is in RAM. */
    if (!e && (addr & 0x3FFFFFFFu) >= 0x08000000u && (addr & 0x3FFFFFFFu) < 0x0A000000u && !(addr & 3)) {
        rt_interpret(c, addr);
        return;
    }
    if (!e) {
        fprintf(stderr, "[rt] indirect call to unknown address 0x%08X (ra=0x%08X)\n", addr, c->r[31]);
        rt_crash_dump(c);
        abort();
    }
    e->fn(c);
}

/* On a fatal error: registers, shadow call stack and all of PSP RAM to
 * build/crash/ for offline analysis (tools/disasm.py works on ram.bin too). */
void rt_crash_dump(Ctx *c) {
    mkdir("build/crash", 0755);
    FILE *f = fopen("build/crash/ram.bin", "wb");   /* 0x08000000-0x0A000000 */
    if (f) { fwrite(rt_ptr(0x08000000u), 1, 0x02000000u, f); fclose(f); }
    f = fopen("build/crash/regs.txt", "w");
    if (f) {
        for (int i = 0; i < 32; i++) fprintf(f, "r%-2d = 0x%08X\n", i, c->r[i]);
        fprintf(f, "hi = 0x%08X lo = 0x%08X\n", c->hi, c->lo);
        fclose(f);
    }
    fprintf(stderr, "[rt] crash dump in build/crash/ (ram.bin = PSP RAM from 0x08000000). Call chain:\n");
    rt_shadow_print(rt_shadow, "      ");
}

void rt_bad_jump(Ctx *c, uint32_t pc, uint32_t target) {
    fprintf(stderr, "[rt] unresolved jump at 0x%08X -> 0x%08X (ra=0x%08X)\n", pc, target, c->r[31]);
    rt_crash_dump(c);
    abort();
}

void rt_break(Ctx *c, uint32_t pc, uint32_t code) {
    (void)c;
    /* GCC emits `break 7` after a division by zero check. Real hardware would
     * raise an exception; log it and keep going. */
    fprintf(stderr, "[rt] break %u at 0x%08X\n", code, pc);
}

void rt_syscall(Ctx *c, uint32_t pc, uint32_t code) {
    (void)c;
    fprintf(stderr, "[rt] raw syscall 0x%X at 0x%08X (imports should go through HLE stubs)\n", code, pc);
    abort();
}

void rt_unimplemented(Ctx *c, uint32_t pc, uint32_t word, const char *op) {
    (void)c;
    fprintf(stderr, "[rt] unimplemented instruction %s (0x%08X) at 0x%08X\n", op, word, pc);
    abort();
}

/* Unimplemented HLE calls: print the first few of each, count the rest.
 * `name` is a string literal from hle_stubs.c, so its address identifies it. */
#define MAX_UNIMPL 256
static struct { const char *name; unsigned count; } g_unimpl[MAX_UNIMPL];

void rt_hle_unimplemented(Ctx *c, const char *name) {
    int i;
    for (i = 0; i < MAX_UNIMPL && g_unimpl[i].name && g_unimpl[i].name != name; i++) {}
    if (i < MAX_UNIMPL) {
        g_unimpl[i].name = name;
        if (g_unimpl[i].count++ >= 3) return;
    }
    fprintf(stderr, "[hle] UNIMPLEMENTED %s(0x%X, 0x%X, 0x%X, 0x%X) ra=0x%08X%s\n",
            name, c->r[4], c->r[5], c->r[6], c->r[7], c->r[31],
            i < MAX_UNIMPL && g_unimpl[i].count == 3 ? "  (further calls not shown)" : "");
}

void rt_hle_report(void) {
    if (!g_unimpl[0].name) return;
    fprintf(stderr, "[hle] unimplemented functions called:\n");
    for (int i = 0; i < MAX_UNIMPL && g_unimpl[i].name; i++)
        fprintf(stderr, "  %6u  %s\n", g_unimpl[i].count, g_unimpl[i].name);
}
