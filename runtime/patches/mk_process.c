/*
 * Midway's process system, reimplemented with fibers.
 *
 * MK Unchained runs most game objects as "processes": coroutines that sleep
 * mid-function and are resumed by a scheduler, one call to run() per process
 * per tick. The original is hand-written MIPS assembly (0x1DC614-0x1DD210) in
 * three flavours (C++ classes picked at runtime):
 *
 *   simple   run 0x1DC644, chain 0x1DC614              (no mid-function sleep)
 *   copying  run 0x1DC794, sleep 0x1DCA98, chain 0x1DC764
 *            processes run on the main stack; sleeping copies their frames out
 *   private  run 0x1DCD3C, sleep 0x1DD024, chain 0x1DCD08,
 *            to/from main stack 0x1DCCC8/0x1DCCE8; each process has its own stack
 *
 * They "return" past several callers and jump into the middle of functions,
 * which plain recompiled C cannot do. Instead each process gets a host fiber
 * (own C stack) and a private guest stack: sleep = switch to the scheduler,
 * resume = switch back, and the C call chain survives in between.
 *
 * Process struct fields used (offsets into the struct at *CUR_PROC):
 *   +0 class/vtable (+16 = kill function)  +4 alive flag  +12 sleep time (float)
 *   +120 hook  +124 entry function  +128 resume address  +140 stack top  +144 saved sp
 */
#include "../fiber.h"
#include "../hle/kernel.h"

#include <string.h>
#include <sys/mman.h>

/* Game globals (module-relative addresses, see lesson 06b). */
#define G(off) (rt_image_base + (off))
#define CUR_PROC      G(0x58E900)   /* the process being run */
#define SCHED_MAIN    G(0x418500)   /* sp saved by "to main stack"; non-zero while there */
#define SCHED_SP      G(0x418504)   /* scheduler's sp during run() */
#define SCHED_RA      G(0x418508)   /* scheduler's return address during run() */
#define SLEEP_DEFAULT G(0x58EEC0)   /* float stored as sleep time by sleep() */
#define KILL_FLAG     G(0x59F258)

enum { P_ALIVE = 4, P_SLEEP = 12, P_HOOK = 120, P_ENTRY = 124, P_RESUME = 128, P_STACKTOP = 140, P_SAVEDSP = 144 };

#define HOST_STACK (512u << 10)
#define GUEST_STACK 0x10000u
#define GUEST_STACKS_BASE 0x0C000000u   /* beyond PSP RAM, inside our 1 GiB mapping */
#define MAX_PROCS 1024

typedef struct {
    uint32_t proc;            /* guest address of the process struct */
    int live;                 /* has a suspended call chain (it slept) */
    Fiber fiber;
    void *host_stack;
    uint32_t guest_stack_top;
    Ctx ctx;                  /* the process's own registers */
    RtShadow shadow;          /* its recompiled call chain (debugging) */
} ProcFiber;

enum Exit { EXIT_SLEEP, EXIT_RETURN, EXIT_CHAIN };

static ProcFiber *g_procs[MAX_PROCS];
static int g_nprocs;
static ProcFiber *g_cur;      /* fiber running process code, NULL while in the scheduler */
static Fiber g_sched;         /* where run() waits while a process runs */
static enum Exit g_exit;
static float g_exit_f0;

static float lwf(uint32_t a) { uint32_t w = MEM_LW(a); float f; memcpy(&f, &w, 4); return f; }
static void swf(uint32_t a, float f) { uint32_t w; memcpy(&w, &f, 4); MEM_SW(a, w); }

/* The VFPU register file is shared CPU state on the PSP: the original
 * process switch never saves it, so it carries over between processes. */
static void share_vfpu(Ctx *to, const Ctx *from) {
    memcpy(to->v, from->v, sizeof to->v);
    memcpy(to->vctrl, from->vctrl, sizeof to->vctrl);
    to->fcr31 = from->fcr31;
}

static ProcFiber *fiber_for(uint32_t proc) {
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i]->proc == proc) return g_procs[i];
    if (g_nprocs == MAX_PROCS) {
        fprintf(stderr, "[proc] more than %d processes\n", MAX_PROCS);
        abort();
    }
    ProcFiber *f = calloc(1, sizeof *f);
    f->proc = proc;
    f->host_stack = mmap(NULL, HOST_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    f->guest_stack_top = GUEST_STACKS_BASE + (uint32_t)(g_nprocs + 1) * GUEST_STACK - 64;
    g_procs[g_nprocs++] = f;
    if ((g_nprocs & (g_nprocs - 1)) == 0) TRACE_NOCTX("(%d processes so far)", g_nprocs);
    return f;
}

static void fiber_main(void *arg) {
    ProcFiber *f = arg;
    uint32_t entry = MEM_LW(MEM_LW(CUR_PROC) + P_ENTRY);
    rt_call_indirect(&f->ctx, entry);
    /* The process function returned: its float result says when to run again. */
    g_exit = EXIT_RETURN;
    g_exit_f0 = f->ctx.f[0];
    fiber_switch(&f->fiber, &g_sched);
    abort();   /* a finished fiber is restarted from scratch, never resumed */
}

/* MKU_TRACE: how many run() calls per second, to see if the game logic is alive. */
static uint64_t g_runs, g_runs_reported_at;
static int g_list_next;
extern uint64_t now_us(void);

/* run(): advance the current process by one tick. */
void proc_run(Ctx *c) {
    g_runs++;
    if (g_trace && now_us() - g_runs_reported_at > 1000000u) {
        fprintf(stderr, "[proc] %llu run() calls so far, %d processes known\n", (unsigned long long)g_runs, g_nprocs);
        g_runs_reported_at = now_us();
        g_list_next = 1;
    }
    if (g_list_next) {   /* once a second: list the processes run in one tick */
        uint32_t p = MEM_LW(CUR_PROC);
        static uint32_t first;
        if (!first) first = p;
        else if (p == first) { g_list_next = 0; first = 0; }
        if (g_list_next)
            fprintf(stderr, "[proc]   0x%08X entry=func_%08X resume=0x%08X sleep=%g\n", p,
                    MEM_LW(p + P_ENTRY) - rt_image_base, MEM_LW(p + P_RESUME), lwf(p + P_SLEEP));
    }
    MEM_SW(SCHED_RA, c->r[31]);
    MEM_SW(SCHED_SP, c->r[29]);
    for (;;) {
        ProcFiber *f = fiber_for(MEM_LW(CUR_PROC));
        if (!f->live) {   /* (re)start: fresh registers, empty stacks */
            f->ctx = *c;
            f->ctx.r[29] = f->guest_stack_top;
            f->ctx.r[31] = 0;
            fiber_init(&f->fiber, f->host_stack, HOST_STACK, fiber_main, f);
        } else {
            share_vfpu(&f->ctx, c);
        }
        g_cur = f;
        RtShadow *sched_shadow = rt_shadow;
        if (!f->live) f->shadow.depth = 0;
        rt_shadow = &f->shadow;
        fiber_switch(&g_sched, &f->fiber);
        rt_shadow = sched_shadow;
        g_cur = NULL;
        share_vfpu(c, &f->ctx);

        if (g_exit == EXIT_SLEEP) {   /* it slept: resume it next time */
            f->live = 1;
            c->f[0] = lwf(SLEEP_DEFAULT);
            return;
        }

        /* It returned (or chained to a new entry) with a sleep time in f0. */
        f->live = 0;
        float f0 = g_exit_f0;
        uint32_t p = MEM_LW(CUR_PROC);
        MEM_SW(p + P_SAVEDSP, MEM_LW(p + P_STACKTOP));
        if (f0 == 0.0f) continue;   /* 0 = run (the new) entry again right now */

        swf(MEM_LW(CUR_PROC) + P_SLEEP, f0);
        c->f[0] = f0;
        uint32_t hook = MEM_LW(MEM_LW(CUR_PROC) + P_HOOK);
        if (hook) {
            rt_call_indirect(c, hook);
            if (MEM_LW(MEM_LW(CUR_PROC) + P_ALIVE) == 0) return;
        }
        float sleep = lwf(MEM_LW(CUR_PROC) + P_SLEEP);
        if (0.0f <= sleep) return;
        /* A negative sleep time means "kill me": call the class's kill function. */
        uint32_t cur = MEM_LW(CUR_PROC);
        uint32_t kill = MEM_LW(MEM_LW(cur) + 16);
        MEM_SW(KILL_FLAG, 0);
        c->r[4] = cur;
        rt_call_indirect(c, kill);
        return;
    }
}

/* sleep(): suspend the running process until its next run(). */
void proc_sleep(Ctx *pc) {
    ProcFiber *f = g_cur;
    uint32_t p = MEM_LW(CUR_PROC);
    if (!f) {
        LOG(" outside a process (ra=0x%08X) -- ignored", pc->r[31]);
        return;
    }
    MEM_SW(p + P_RESUME, pc->r[31]);
    MEM_SW(p + P_SAVEDSP, MEM_LW(p + P_STACKTOP) - 16);   /* "has a saved context" */
    MEM_SW(p + P_SLEEP, MEM_LW(SLEEP_DEFAULT));
    pc->fi[0] = MEM_LW(SLEEP_DEFAULT);
    uint32_t hook = MEM_LW(p + P_HOOK);
    if (hook) rt_call_indirect(pc, hook);
    g_exit = EXIT_SLEEP;
    fiber_switch(&f->fiber, &g_sched);
    /* resumed by proc_run(): carry on in the caller */
}

/* chain(func, sleep): replace the process's code with func and drop its whole
 * current call chain, as if the old entry had returned `sleep`. */
void proc_chain(Ctx *pc) {
    ProcFiber *f = g_cur;
    MEM_SW(MEM_LW(CUR_PROC) + P_ENTRY, pc->r[4]);
    if (!f) {
        LOG("(0x%08X) outside a process -- entry set only", pc->r[4]);
        pc->f[0] = pc->f[12];
        return;
    }
    g_exit = EXIT_CHAIN;
    g_exit_f0 = pc->f[12];
    fiber_switch(&f->fiber, &g_sched);
    abort();   /* never resumed */
}

/* The private-stack flavour can hop onto the big main stack for deep calls.
 * Our guest stacks are already 64 KiB, so only the "on main stack" marker is kept. */
void proc_to_main_stack(Ctx *c) { MEM_SW(SCHED_MAIN, c->r[29]); }
void proc_from_main_stack(Ctx *c) { (void)c; MEM_SW(SCHED_MAIN, 0); }

/* Debug dump (called on MKU_TIMEOUT): every process we have a fiber for. */
void proc_dump(void) {
    fprintf(stderr, "[proc] boot sequencer step counter (0x3AE020) = %u\n", MEM_LW(G(0x3AE020)));
    fprintf(stderr, "[proc] %d processes:\n", g_nprocs);
    for (int i = 0; i < g_nprocs; i++) {
        ProcFiber *f = g_procs[i];
        uint32_t p = f->proc;
        fprintf(stderr, "  0x%08X %s entry=func_%08X sleep=%g alive=%u class_vt=0x%08X\n", p,
                f->live ? "SUSPENDED" : "idle     ", MEM_LW(p + P_ENTRY) - rt_image_base, lwf(p + P_SLEEP),
                MEM_LW(p + P_ALIVE), MEM_LW(p) - rt_image_base);
        if (f->live) rt_shadow_print(&f->shadow, "      ");
    }
}

/* A longjmp is leaving the running process for good (rt_longjmp): forget its
 * call chain so it restarts from scratch if it is ever run again. */
void proc_abandon_current(void) {
    if (!g_cur) return;
    g_cur->live = 0;
    g_cur = NULL;
}
