/*
 * Guest setjmp/longjmp on top of host setjmp/longjmp.
 *
 * MK switches game modes by longjmp-ing back into main() (func_000C0EF8).
 * The recompiler turns each call to the game's setjmp into RT_SETJMP(c),
 * a host _setjmp in the caller's frame; rt_longjmp() restores the guest
 * registers the way the game's own longjmp does and jumps there.
 *
 * Guest jmp_buf layout (the game's setjmp at 0x247D84):
 *   s0-s7 (0-28), sp (32), fp (36), ra (40), f20-f31 (44-88)
 */
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>

#define MAX_JMPBUFS 16

static struct {
    uint32_t guest;     /* guest jmp_buf address */
    jmp_buf host;
    Ctx *ctx;           /* registers of the thread that called setjmp */
    RtShadow *shadow;
    int shadow_depth;
} g_bufs[MAX_JMPBUFS];

static int slot_for(uint32_t guest) {
    for (int i = 0; i < MAX_JMPBUFS; i++)
        if (g_bufs[i].guest == guest) return i;
    for (int i = 0; i < MAX_JMPBUFS; i++)
        if (!g_bufs[i].guest) { g_bufs[i].guest = guest; return i; }
    fprintf(stderr, "[rt] too many setjmp buffers\n");
    abort();
}

jmp_buf *rt_setjmp_buf(uint32_t guest) { return &g_bufs[slot_for(guest)].host; }

/* First return of setjmp: store the guest registers like the original does. */
void rt_setjmp_save(Ctx *c) {
    uint32_t b = c->r[4];
    int s = slot_for(b);
    for (int i = 0; i < 8; i++) MEM_SW(b + 4u * (uint32_t)i, c->r[16 + i]);
    MEM_SW(b + 32, c->r[29]);
    MEM_SW(b + 36, c->r[30]);
    MEM_SW(b + 40, c->r[31]);
    for (int i = 0; i < 12; i++) MEM_SW(b + 44 + 4u * (uint32_t)i, c->fi[20 + i]);
    g_bufs[s].ctx = c;
    g_bufs[s].shadow = rt_shadow;
    g_bufs[s].shadow_depth = rt_shadow->depth;
    c->r[2] = 0;
}

void proc_abandon_current(void);   /* patches/mk_process.c */

/* longjmp(buf, value): replaces the game's longjmp (config/patches.txt). */
void rt_longjmp(Ctx *c) {
    uint32_t b = c->r[4], value = c->r[5] ? c->r[5] : 1;
    int s = slot_for(b);
    Ctx *t = g_bufs[s].ctx;
    if (!t) {
        fprintf(stderr, "[rt] longjmp to a buffer that was never set (0x%08X)\n", b);
        abort();
    }
    for (int i = 0; i < 8; i++) t->r[16 + i] = MEM_LW(b + 4u * (uint32_t)i);
    t->r[29] = MEM_LW(b + 32);
    t->r[30] = MEM_LW(b + 36);
    t->r[31] = MEM_LW(b + 40);
    for (int i = 0; i < 12; i++) t->fi[20 + i] = MEM_LW(b + 44 + 4u * (uint32_t)i);
    if (t != c) {   /* the VFPU/FPU control state is shared CPU state */
        memcpy(t->v, c->v, sizeof t->v);
        memcpy(t->vctrl, c->vctrl, sizeof t->vctrl);
        t->fcr31 = c->fcr31;
    }
    t->r[2] = value;
    proc_abandon_current();   /* jumping out of a process fiber: it never resumes */
    rt_shadow = g_bufs[s].shadow;
    rt_shadow->depth = g_bufs[s].shadow_depth;
    _longjmp(g_bufs[s].host, 1);
}
