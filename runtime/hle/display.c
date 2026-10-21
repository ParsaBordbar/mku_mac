/* sceDisplay, vblank interrupts (InterruptManager), sceCtrl. (sceGe_user: gpu/ge.c) */
#include "kernel.h"
#include "../host/host.h"

void present_frame_shown(uint64_t frame);
void display_vblank(void);

#include <string.h>

/* ------------------------------------------------------------------------ */
/* vblank: the PSP screen refreshes at 59.94 Hz */

int g_in_interrupt;

uint64_t vblank_count(void) { return now_us() / VBLANK_US; }
uint64_t next_vblank_us(void) { return (vblank_count() + 1) * VBLANK_US; }

#define PSP_VBLANK_INT 30
static struct { uint32_t handler, arg; int enabled; } g_vblank[16];
static uint64_t g_vblank_serviced;
static uint32_t g_intr_stack;

/* Run the game's vblank interrupt handlers once per vblank. Called from the
 * scheduler, so handlers run whenever some thread is blocked -- which, for a
 * game that waits for vblank every frame, is every frame. */
void kernel_run_interrupts(void) {
    if (g_in_interrupt) return;
    uint64_t vc = vblank_count();
    if (vc == g_vblank_serviced) return;
    g_vblank_serviced = vc;
    display_vblank();
    for (uint32_t sub = 0; sub < 16; sub++) {
        if (!g_vblank[sub].enabled || !g_vblank[sub].handler) continue;
        if (!g_intr_stack) g_intr_stack = mem_alloc(0x4000, 1, 256) + 0x4000;
        Ctx ic;
        rt_ctx_init(&ic);
        ic.r[4] = sub;
        ic.r[5] = g_vblank[sub].arg;
        ic.r[29] = g_intr_stack - 16;
        ic.r[28] = g_current ? g_current->ctx.r[28] : 0;
        static RtShadow isr_shadow;
        RtShadow *saved = rt_shadow;
        rt_shadow = &isr_shadow;
        isr_shadow.depth = 0;
        g_in_interrupt = 1;
        rt_call_indirect(&ic, g_vblank[sub].handler);
        g_in_interrupt = 0;
        rt_shadow = saved;
    }
}

void hle_sceKernelRegisterSubIntrHandler(Ctx *c) {
    TRACE("(int=%u, sub=%u, handler=0x%08X, arg=0x%X)", A0, A1, A2, A3);
    if (A0 == PSP_VBLANK_INT && A1 < 16) {
        g_vblank[A1].handler = A2;
        g_vblank[A1].arg = A3;
    } else {
        LOG("(int=%u) -- only vblank (30) is supported", A0);
    }
    RET(0);
}

void hle_sceKernelReleaseSubIntrHandler(Ctx *c) {
    if (A0 == PSP_VBLANK_INT && A1 < 16) memset(&g_vblank[A1], 0, sizeof g_vblank[A1]);
    RET(0);
}

void hle_sceKernelEnableSubIntr(Ctx *c) {
    TRACE("(int=%u, sub=%u)", A0, A1);
    if (A0 == PSP_VBLANK_INT && A1 < 16) g_vblank[A1].enabled = 1;
    RET(0);
}

/* ------------------------------------------------------------------------ */
/* sceDisplay */

static uint32_t g_fb_addr, g_fb_width, g_fb_format;
static uint64_t g_frames, g_last_report_us;

void hle_sceDisplaySetMode(Ctx *c) { TRACE("(%u, %u, %u)", A0, A1, A2); RET(0); }

/* sync=0 (IMMEDIATE) shows the buffer now; sync=1 (NEXTFRAME) latches it and
 * it is shown at the next vblank. MK calls both each frame (NEXTFRAME, then
 * IMMEDIATE with the buffer it wants shown), like PPSSPP's __DisplaySetFramebuf
 * the immediate call also replaces the latched one. Frames go to the window
 * only when the displayed buffer actually changes. */
static struct { uint32_t addr, width, format; int pending; uint64_t vc; } g_latch;

static void show(uint32_t addr, uint32_t width, uint32_t format) {
    g_fb_addr = addr;
    g_fb_width = width;
    g_fb_format = format;
    present_frame_shown(g_frames);
}

void display_vblank(void) {   /* apply a latched framebuffer once its vblank has come */
    if (g_latch.pending && vblank_count() > g_latch.vc) {
        g_latch.pending = 0;
        show(g_latch.addr, g_latch.width, g_latch.format);
    }
}

void hle_sceDisplaySetFrameBuf(Ctx *c) {
    if (!g_frames) fprintf(stderr, "[display] first framebuffer: 0x%08X width %u format %u\n", A0, A1, A2);
    if (getenv("MKU_VBTRACE")) fprintf(stderr, "[vb] setframebuf 0x%08X sync=%u from 0x%08X vc=%llu t=%.2f\n", A0, A3, c->r[31], (unsigned long long)vblank_count(), now_us() / 1000.0);
    display_vblank();
    g_frames++;
    if (A3 == 0) {   /* IMMEDIATE */
        g_latch.pending = 0;
        show(A0, A1, A2);
    } else {
        g_latch.addr = A0; g_latch.width = A1; g_latch.format = A2;
        g_latch.pending = 1;
        g_latch.vc = vblank_count();
        g_fb_width = A1; g_fb_format = A2;
    }
    uint64_t now = now_us();
    if (now - g_last_report_us >= 1000000u) {
        fprintf(stderr, "[display] %llu frames shown, %.1f s\n", (unsigned long long)g_frames, now / 1e6);
        g_last_report_us = now;
    }
    RET(0);
}

uint64_t display_frame_count(void) { return g_frames; }

void display_framebuffer(uint32_t *addr, uint32_t *width, uint32_t *format) {
    *addr = g_fb_addr;
    *width = g_fb_width;
    *format = g_fb_format;
}

void hle_sceDisplayGetFrameBuf(Ctx *c) {
    if (A0) MEM_SW(A0, g_fb_addr);
    if (A1) MEM_SW(A1, g_fb_width);
    if (A2) MEM_SW(A2, g_fb_format);
    RET(0);
}

void hle_sceDisplayWaitVblankStart(Ctx *c) {
    RET(sched_wait(g_current, WAIT_DELAY, 0, next_vblank_us() - now_us() + 1, 0));
}
void hle_sceDisplayWaitVblankStartCB(Ctx *c) { kernel_run_callbacks(c); hle_sceDisplayWaitVblankStart(c); }
void hle_sceDisplayGetVcount(Ctx *c) { RET(vblank_count()); }
void hle_sceDisplayGetFramePerSec(Ctx *c) { c->f[0] = 59.9400599f; }   /* returns a float: $f0 */

/* ------------------------------------------------------------------------ */
/* sceCtrl: buttons and analog stick from the host window (keyboard / controller). */

void hle_sceCtrlSetSamplingMode(Ctx *c) { RET(0); }

void hle_sceCtrlReadBufferPositive(Ctx *c) {
    uint32_t data = A0, count = A1 ? A1 : 1;
    sched_wait(g_current, WAIT_DELAY, 0, next_vblank_us() - now_us() + 1, 0);   /* blocks until next sample */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = data + 16 * i;   /* SceCtrlData: timestamp, buttons, lx, ly, reserved[6] */
        memset(rt_ptr(p), 0, 16);
        MEM_SW(p, (uint32_t)now_us());
        MEM_SW(p + 4, host_buttons());
        MEM_SB(p + 8, host_analog() & 0xFF);
        MEM_SB(p + 9, host_analog() >> 8);
    }
    RET(count);
}
