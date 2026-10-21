/* ThreadManForUser: semaphores, event flags, fixed pools, callbacks. */
#include "kernel.h"

#include <string.h>

extern Thread *g_current;

/* Waiters are found by scanning threads for (wait_type, wait_id); ready_seq
 * holds the order they started waiting in, so the oldest goes first. */
#define MAX_SCAN 128
static int collect_waiters(enum WaitType type, int id, Thread **out);

/* ------------------------------------------------------------------------ */
/* semaphores */

typedef struct { char name[32]; uint32_t attr; int count, max; } Sema;

void hle_sceKernelCreateSema(Ctx *c) {
    Sema *s = calloc(1, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", GSTR(A0));
    s->attr = A1;
    s->count = (int)A2;
    s->max = (int)A3;
    int uid = kobj_new(OBJ_SEMA, s);
    TRACE("(\"%s\", init=%d, max=%d) -> 0x%X", s->name, s->count, s->max, uid);
    RET(uid);
}

void hle_sceKernelDeleteSema(Ctx *c) {
    Sema *s = kobj_get((int)A0, OBJ_SEMA);
    if (!s) RET(ERR_NOT_FOUND_SEMA);
    Thread *w[MAX_SCAN];
    int n = collect_waiters(WAIT_SEMA, (int)A0, w);
    for (int i = 0; i < n; i++) sched_wake(w[i], ERR_WAIT_DELETE);
    kobj_free((int)A0);
    free(s);
    c->r[2] = 0;
    sched_yield_if_preempted();
}

static void sema_wake_waiters(int uid, Sema *s) {
    Thread *w[MAX_SCAN];
    int n = collect_waiters(WAIT_SEMA, uid, w);
    for (int i = 0; i < n && s->count > 0; i++) {
        int need = (int)w[i]->wait_arg[0];
        if (need > s->count) break;   /* FIFO: don't let later waiters overtake */
        s->count -= need;
        sched_wake(w[i], 0);
    }
}

void hle_sceKernelSignalSema(Ctx *c) {
    Sema *s = kobj_get((int)A0, OBJ_SEMA);
    if (!s) RET(ERR_NOT_FOUND_SEMA);
    TRACE("(\"%s\", +%d) count=%d", s->name, (int)A1, s->count);
    if (s->count + (int)A1 > s->max) RET(ERR_SEMA_OVERFLOW);
    s->count += (int)A1;
    sema_wake_waiters((int)A0, s);
    c->r[2] = 0;
    sched_yield_if_preempted();
}

void hle_sceKernelWaitSema(Ctx *c) {
    Sema *s = kobj_get((int)A0, OBJ_SEMA);
    if (!s) RET(ERR_NOT_FOUND_SEMA);
    int need = (int)A1;
    TRACE("(\"%s\", %d) count=%d", s->name, need, s->count);
    if (s->count >= need) { s->count -= need; RET(0); }
    g_current->wait_arg[0] = (uint32_t)need;
    uint32_t timeout = A2 ? MEM_LW(A2) : 0;
    RET(sched_wait(g_current, WAIT_SEMA, (int)A0, timeout, A2));
}
void hle_sceKernelWaitSemaCB(Ctx *c) { kernel_run_callbacks(c); hle_sceKernelWaitSema(c); }

void hle_sceKernelPollSema(Ctx *c) {
    Sema *s = kobj_get((int)A0, OBJ_SEMA);
    if (!s) RET(ERR_NOT_FOUND_SEMA);
    if (s->count >= (int)A1) { s->count -= (int)A1; RET(0); }
    RET(ERR_SEMA_ZERO);
}

/* ------------------------------------------------------------------------ */
/* event flags: a 32-bit word threads can wait on (all bits / any bit) */

typedef struct { char name[32]; uint32_t attr, init, bits; } EventFlag;

#define EVF_OR 0x01
#define EVF_CLEARALL 0x10
#define EVF_CLEAR 0x20

static int evf_match(uint32_t bits, uint32_t want, uint32_t mode) {
    return (mode & EVF_OR) ? (bits & want) != 0 : (bits & want) == want;
}

static void evf_consume(EventFlag *e, uint32_t want, uint32_t mode, uint32_t out_addr) {
    if (out_addr) MEM_SW(out_addr, e->bits);
    if (mode & EVF_CLEARALL) e->bits = 0;
    else if (mode & EVF_CLEAR) e->bits &= ~want;
}

void hle_sceKernelCreateEventFlag(Ctx *c) {
    EventFlag *e = calloc(1, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", GSTR(A0));
    e->attr = A1;
    e->init = e->bits = A2;
    int uid = kobj_new(OBJ_EVF, e);
    TRACE("(\"%s\", bits=0x%X) -> 0x%X", e->name, e->bits, uid);
    RET(uid);
}

void hle_sceKernelDeleteEventFlag(Ctx *c) {
    EventFlag *e = kobj_get((int)A0, OBJ_EVF);
    if (!e) RET(ERR_NOT_FOUND_EVF);
    Thread *w[MAX_SCAN];
    int n = collect_waiters(WAIT_EVF, (int)A0, w);
    for (int i = 0; i < n; i++) sched_wake(w[i], ERR_WAIT_DELETE);
    kobj_free((int)A0);
    free(e);
    c->r[2] = 0;
    sched_yield_if_preempted();
}

void hle_sceKernelSetEventFlag(Ctx *c) {
    EventFlag *e = kobj_get((int)A0, OBJ_EVF);
    if (!e) RET(ERR_NOT_FOUND_EVF);
    e->bits |= A1;
    TRACE("(\"%s\", 0x%X) -> 0x%X", e->name, A1, e->bits);
    Thread *w[MAX_SCAN];
    int n = collect_waiters(WAIT_EVF, (int)A0, w);
    for (int i = 0; i < n; i++) {
        uint32_t want = w[i]->wait_arg[0], mode = w[i]->wait_arg[1];
        if (evf_match(e->bits, want, mode)) {
            evf_consume(e, want, mode, w[i]->wait_arg[2]);
            sched_wake(w[i], 0);
        }
    }
    c->r[2] = 0;
    sched_yield_if_preempted();
}

/* Note: the PSP's "clear" ANDs with the argument -- you pass the bits to KEEP. */
void hle_sceKernelClearEventFlag(Ctx *c) {
    EventFlag *e = kobj_get((int)A0, OBJ_EVF);
    if (!e) RET(ERR_NOT_FOUND_EVF);
    e->bits &= A1;
    TRACE("(\"%s\", &0x%X) -> 0x%X", e->name, A1, e->bits);
    RET(0);
}

void hle_sceKernelWaitEventFlag(Ctx *c) {
    EventFlag *e = kobj_get((int)A0, OBJ_EVF);
    if (!e) RET(ERR_NOT_FOUND_EVF);
    uint32_t want = A1, mode = A2, out = A3, timeout_addr = T0;
    TRACE("(\"%s\", want=0x%X, mode=0x%X) bits=0x%X", e->name, want, mode, e->bits);
    if (evf_match(e->bits, want, mode)) {
        evf_consume(e, want, mode, out);
        RET(0);
    }
    Thread *t = g_current;
    t->wait_arg[0] = want;
    t->wait_arg[1] = mode;
    t->wait_arg[2] = out;
    uint32_t timeout = timeout_addr ? MEM_LW(timeout_addr) : 0;
    RET(sched_wait(t, WAIT_EVF, (int)A0, timeout, timeout_addr));
}

void hle_sceKernelReferEventFlagStatus(Ctx *c) {
    EventFlag *e = kobj_get((int)A0, OBJ_EVF);
    if (!e) RET(ERR_NOT_FOUND_EVF);
    uint32_t info = A1, size = MEM_LW(info);
    Thread *w[MAX_SCAN];
    int n = collect_waiters(WAIT_EVF, (int)A0, w);
    /* SceKernelEventFlagInfo: size, name[32], attr, initPattern, currentPattern, numWaitThreads */
    if (size >= 36) memcpy(rt_ptr(info + 4), e->name, 32);
    uint32_t words[] = {e->attr, e->init, e->bits, (uint32_t)n};
    for (uint32_t i = 0; i < 4 && 36 + 4 * i + 4 <= size; i++) MEM_SW(info + 36 + 4 * i, words[i]);
    RET(0);
}

/* ------------------------------------------------------------------------ */
/* fixed-size memory pools */

typedef struct { char name[32]; uint32_t base, block_size, count; uint8_t *used; } Fpl;

void hle_sceKernelCreateFpl(Ctx *c) {
    Fpl *f = calloc(1, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", GSTR(A0));
    /* args: name, partition, attr, block size, block count, opt */
    f->block_size = (A3 + 3) & ~3u;
    f->count = T0;
    f->base = mem_alloc(f->block_size * f->count, 0, 16);
    if (!f->base) { free(f); RET(ERR_NO_MEMORY); }
    f->used = calloc(f->count, 1);
    int uid = kobj_new(OBJ_FPL, f);
    TRACE("(\"%s\", %u x %u) -> 0x%X at 0x%08X", f->name, f->count, f->block_size, uid, f->base);
    RET(uid);
}

void hle_sceKernelDeleteFpl(Ctx *c) {
    Fpl *f = kobj_get((int)A0, OBJ_FPL);
    if (!f) RET(ERR_NOT_FOUND_FPL);
    mem_free(f->base);
    free(f->used);
    free(f);
    kobj_free((int)A0);
    RET(0);
}

void hle_sceKernelTryAllocateFpl(Ctx *c) {
    Fpl *f = kobj_get((int)A0, OBJ_FPL);
    if (!f) RET(ERR_NOT_FOUND_FPL);
    for (uint32_t i = 0; i < f->count; i++) {
        if (!f->used[i]) {
            f->used[i] = 1;
            MEM_SW(A1, f->base + i * f->block_size);
            RET(0);
        }
    }
    RET(ERR_NO_MEMORY);
}

void hle_sceKernelFreeFpl(Ctx *c) {
    Fpl *f = kobj_get((int)A0, OBJ_FPL);
    if (!f) RET(ERR_NOT_FOUND_FPL);
    uint32_t i = (A1 - f->base) / f->block_size;
    if (A1 < f->base || i >= f->count) RET(ERR_ILLEGAL_ARGUMENT);
    f->used[i] = 0;
    RET(0);
}

/* ------------------------------------------------------------------------ */
/* callbacks: functions the system "notifies" (memory stick inserted, exit
 * button, power events). They run on the thread that created them, but only
 * while that thread waits in a *CB function (DelayThreadCB, SleepThreadCB...).
 * Signature: int callback(int notify_count, int notify_arg, void *common). */

typedef struct {
    char name[32];
    uint32_t func, common;
    int owner;                 /* thread uid */
    int uid, notify_count;
    uint32_t notify_arg;
} Callback;

#define MAX_CALLBACKS 64
static Callback *g_cbs[MAX_CALLBACKS];

void hle_sceKernelCreateCallback(Ctx *c) {
    Callback *cb = calloc(1, sizeof *cb);
    snprintf(cb->name, sizeof cb->name, "%s", GSTR(A0));
    cb->func = A1;
    cb->common = A2;
    cb->owner = g_current->uid;
    cb->uid = kobj_new(OBJ_CALLBACK, cb);
    for (int i = 0; i < MAX_CALLBACKS; i++) if (!g_cbs[i]) { g_cbs[i] = cb; break; }
    TRACE("(\"%s\", func=0x%08X) -> 0x%X", cb->name, cb->func, cb->uid);
    RET(cb->uid);
}

static void callback_delete(Callback *cb) {
    for (int i = 0; i < MAX_CALLBACKS; i++) if (g_cbs[i] == cb) g_cbs[i] = NULL;
    kobj_free(cb->uid);
    free(cb);
}

void hle_sceKernelDeleteCallback(Ctx *c) {
    Callback *cb = kobj_get((int)A0, OBJ_CALLBACK);
    if (!cb) RET(ERR_NOT_FOUND_CALLBACK);
    callback_delete(cb);
    RET(0);
}

/* The system notifies a callback; if its owner is sleeping in a *CB wait,
 * wake it so it can run the callback. */
void kernel_notify_callback(int uid, uint32_t arg) {
    Callback *cb = kobj_get(uid, OBJ_CALLBACK);
    if (!cb) return;
    cb->notify_count++;
    cb->notify_arg = arg;
    TRACE_NOCTX(" \"%s\" arg=%u", cb->name, arg);
    Thread *t = thread_by_uid(cb->owner);
    if (t && t != g_current && t->state == TS_WAITING && t->in_cb_wait) sched_wake(t, WAKE_FOR_CALLBACK);
}

/* Run the current thread's notified callbacks, on a copy of its registers just
 * below its stack pointer. Returns how many ran. */
int kernel_run_callbacks(Ctx *c) {
    int ran = 0;
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        Callback *cb = g_cbs[i];
        if (!cb || !cb->notify_count || cb->owner != g_current->uid) continue;
        int count = cb->notify_count;
        cb->notify_count = 0;
        Ctx cc = *c;
        cc.r[29] = (c->r[29] - 256) & ~15u;
        cc.r[4] = (uint32_t)count;
        cc.r[5] = cb->notify_arg;
        cc.r[6] = cb->common;
        cc.r[31] = 0;
        TRACE(" running \"%s\" (count=%d, arg=%u)", cb->name, count, cb->notify_arg);
        rt_call_indirect(&cc, cb->func);
        if (cc.r[2] != 0) callback_delete(cb);   /* non-zero return: delete me */
        ran++;
    }
    return ran;
}

void hle_sceKernelCheckCallback(Ctx *c) { RET(kernel_run_callbacks(c) > 0); }

/* ------------------------------------------------------------------------ */

extern Thread *thread_scan(int i);

static int collect_waiters(enum WaitType type, int id, Thread **out) {
    int n = 0;
    for (int i = 0; i < MAX_SCAN; i++) {
        Thread *t = thread_scan(i);
        if (t && t->state == TS_WAITING && t->wait_type == type && t->wait_id == id) out[n++] = t;
    }
    /* oldest waiter first */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && out[j]->ready_seq < out[j - 1]->ready_seq; j--) {
            Thread *tmp = out[j]; out[j] = out[j - 1]; out[j - 1] = tmp;
        }
    return n;
}
