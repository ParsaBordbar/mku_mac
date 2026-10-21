/*
 * PSP threads on host threads.
 *
 * The PSP has one CPU core and a priority scheduler: the highest-priority
 * ready thread runs until it blocks (or a higher-priority thread wakes up).
 * We give every PSP thread its own host thread, but only the one holding the
 * "baton" -- g_lock + g_current -- may run guest code. Every thread switch
 * therefore happens inside an HLE call, at a point we control, which keeps
 * the game exactly as single-threaded as it was on the PSP.
 */
#include "kernel.h"
#include "../gpu/gpu.h"

void profile_report(void);
void proc_dump(void);

#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int g_trace;
Thread *g_current;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_idle_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_seq;
static struct timespec g_t0;

#define MAX_THREADS 128
static Thread *g_threads[MAX_THREADS];

/* ------------------------------------------------------------------------ */
/* object ids */

#define MAX_OBJ 4096
#define UID_BASE 0x1000
static struct { enum ObjType type; void *ptr; } g_objs[MAX_OBJ];

int kobj_new(enum ObjType type, void *ptr) {
    for (int i = 1; i < MAX_OBJ; i++) {
        if (g_objs[i].type == OBJ_NONE) {
            g_objs[i].type = type;
            g_objs[i].ptr = ptr;
            return UID_BASE + i;
        }
    }
    fprintf(stderr, "[kernel] out of object ids\n");
    abort();
}

void *kobj_get(int uid, enum ObjType type) {
    int i = uid - UID_BASE;
    if (i <= 0 || i >= MAX_OBJ || g_objs[i].type != type) return NULL;
    return g_objs[i].ptr;
}

void kobj_free(int uid) {
    int i = uid - UID_BASE;
    if (i > 0 && i < MAX_OBJ) g_objs[i].type = OBJ_NONE;
}

/* ------------------------------------------------------------------------ */
/* time */

uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)(t.tv_sec - g_t0.tv_sec) * 1000000u + (t.tv_nsec - g_t0.tv_nsec) / 1000;
}

uint64_t g_idle_us;   /* time with every PSP thread blocked (MKU_FPS stats) */

static void idle_until(uint64_t until) {
    uint64_t t0 = now_us();
    struct timespec now, abs;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t wait = until > now_us() ? until - now_us() : 0;
    abs.tv_sec = now.tv_sec + (time_t)(wait / 1000000u);
    abs.tv_nsec = now.tv_nsec + (long)(wait % 1000000u) * 1000;
    if (abs.tv_nsec >= 1000000000) { abs.tv_sec++; abs.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&g_idle_cv, &g_lock, &abs);
    g_idle_us += now_us() - t0;
}

/* ------------------------------------------------------------------------ */
/* scheduler core -- every function here runs with g_lock held */

Thread *thread_scan(int i) { return i >= 0 && i < MAX_THREADS ? g_threads[i] : NULL; }

Thread *thread_by_uid(int uid) {
    if (uid == 0) return g_current;
    return kobj_get(uid, OBJ_THREAD);
}

static void thread_list_add(Thread *t) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!g_threads[i]) { g_threads[i] = t; return; }
    }
    fprintf(stderr, "[kernel] too many threads\n");
    abort();
}

static Thread *best_ready(Thread *exclude) {
    Thread *best = NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = g_threads[i];
        if (!t || t == exclude || (t->state != TS_READY && t->state != TS_RUNNING)) continue;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->ready_seq < best->ready_seq))
            best = t;
    }
    return best;
}

void sched_wake(Thread *t, uint32_t result) {
    if (t->timeout_addr) {
        uint64_t now = now_us();
        MEM_SW(t->timeout_addr, t->wait_until > now ? (uint32_t)(t->wait_until - now) : 0);
    }
    t->state = TS_READY;
    t->wait_type = WAIT_NONE;
    t->wait_result = result;
    t->ready_seq = ++g_seq;
}

static void wake_timeouts(void) {
    uint64_t now = now_us();
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = g_threads[i];
        if (t && t->state == TS_WAITING && t->wait_until && t->wait_until <= now)
            sched_wake(t, t->wait_type == WAIT_DELAY ? 0 : ERR_WAIT_TIMEOUT);
    }
}

static uint64_t earliest_deadline(void) {
    uint64_t best = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = g_threads[i];
        if (t && t->state == TS_WAITING && t->wait_until && (!best || t->wait_until < best))
            best = t->wait_until;
    }
    return best;
}

static void host_start(Thread *t);

static void switch_to(Thread *next) {
    g_current = next;
    next->state = TS_RUNNING;
    if (!next->host_started) host_start(next);
    else pthread_cond_broadcast(&next->cv);
}

static void wait_for_baton(Thread *self, int generation) {
    while (g_current != self || self->generation != generation)
        pthread_cond_wait(&self->cv, &g_lock);
    rt_shadow = &self->shadow;
}

/* Find something to run. If nothing is ready, sleep until the next timeout
 * (or vblank). Returns the chosen thread, already marked running. */
static Thread *pick_and_switch(Thread *self) {
    uint64_t idle_since = 0;
    for (;;) {
        kernel_run_interrupts();
        wake_timeouts();
        Thread *next = best_ready(NULL);
        if (next) {
            if (next == self) { self->state = TS_RUNNING; g_current = self; rt_shadow = &self->shadow; }
            else switch_to(next);
            return next;
        }
        uint64_t until = earliest_deadline(), vb = next_vblank_us();
        if (!until || vb < until) until = vb;
        if (!idle_since) idle_since = now_us();
        else if (now_us() - idle_since > 5000000u) {
            fprintf(stderr, "[kernel] all threads have been waiting for 5 s:\n");
            kernel_dump_threads(stderr);
            idle_since = now_us() + 1000000000ull;   /* report once */
        }
        idle_until(until);
    }
}

static void reschedule(Thread *self) {
    int gen = self->generation;
    if (pick_and_switch(self) != self) wait_for_baton(self, gen);
}

uint32_t sched_wait(Thread *t, enum WaitType type, int id, uint64_t timeout_us, uint32_t timeout_addr) {
    static int wt = -1;
    if (wt < 0) wt = getenv("MKU_WAITTRACE") != NULL;
    if (wt) fprintf(stderr, "[wait] %s type=%d id=%d timeout=%llu ra=0x%08X t=%.2f\n", t->name, type, id,
                    (unsigned long long)timeout_us, t->ctx.r[31], now_us() / 1000.0);
    t->state = TS_WAITING;
    t->wait_type = type;
    t->wait_id = id;
    t->wait_until = timeout_us ? now_us() + timeout_us : 0;
    t->timeout_addr = timeout_addr;
    t->wait_result = 0;
    t->ready_seq = ++g_seq;   /* FIFO order among waiters on the same object */
    reschedule(t);
    return t->wait_result;
}

extern int g_in_interrupt;

void sched_yield_if_preempted(void) {
    Thread *cur = g_current;
    if (g_in_interrupt || !cur) return;
    Thread *best = best_ready(cur);
    if (best && best->prio < cur->prio) {   /* lower number = higher priority */
        cur->state = TS_READY;
        reschedule(cur);
    }
}

void kernel_yield(void) {
    Thread *cur = g_current;
    cur->state = TS_READY;
    cur->ready_seq = ++g_seq;   /* go to the back of the queue for this priority */
    reschedule(cur);
}

/* ------------------------------------------------------------------------ */
/* thread lifecycle */

static void thread_finish(Thread *t, uint32_t status) {
    t->state = TS_DORMANT;
    t->exit_status = status;
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *w = g_threads[i];
        if (w && w->state == TS_WAITING && w->wait_type == WAIT_THREAD_END && w->wait_id == t->uid)
            sched_wake(w, status);
    }
}

/* The calling host thread is done with guest code: pass the baton on. */
static void hand_off(Thread *self) {
    Thread *next = pick_and_switch(self);
    if (next == self) {
        fprintf(stderr, "[kernel] internal error: finished thread rescheduled\n");
        abort();
    }
}

struct HostArg { Thread *t; int generation; };

static void *host_main(void *p) {
    struct HostArg a = *(struct HostArg *)p;
    free(p);
    Thread *t = a.t;
    pthread_mutex_lock(&g_lock);
    wait_for_baton(t, a.generation);
    rt_call_indirect(&t->ctx, t->entry);      /* the thread's whole life */
    if (t->generation == a.generation) {      /* returned normally: same as sceKernelExitThread(v0) */
        thread_finish(t, t->ctx.r[2]);
        hand_off(t);
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void host_start(Thread *t) {
    struct HostArg *a = malloc(sizeof *a);
    a->t = t;
    a->generation = t->generation;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);   /* deep guest call chains = deep host recursion */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->host, &attr, host_main, a)) {
        perror("pthread_create");
        abort();
    }
    t->host_started = 1;
}

static Thread *thread_new(const char *name, uint32_t entry, uint32_t prio, uint32_t attr) {
    Thread *t = calloc(1, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->entry = entry;
    t->init_prio = t->prio = prio;
    t->attr = attr;
    t->state = TS_DORMANT;
    pthread_cond_init(&t->cv, NULL);
    t->uid = kobj_new(OBJ_THREAD, t);
    thread_list_add(t);
    return t;
}

static void thread_delete(Thread *t) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_threads[i] == t) g_threads[i] = NULL;
    kobj_free(t->uid);
    if (t->stack_block) mem_free(t->stack_block);
    t->state = TS_DELETED;
    t->generation++;   /* its host thread (if blocked) never runs again; Thread is leaked on purpose */
}

static const char *state_name(enum ThreadState s) {
    static const char *n[] = {"dormant", "ready", "RUNNING", "waiting", "deleted"};
    return n[s];
}

void kernel_dump_threads(FILE *f) {
    static const char *w[] = {"", "delay", "sleep", "sema", "evf", "thread-end", "suspend"};
    uint64_t now = now_us();
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = g_threads[i];
        if (!t) continue;
        fprintf(f, "  thread 0x%X %-20s prio 0x%02X %-8s", t->uid, t->name, t->prio, state_name(t->state));
        if (t->state == TS_WAITING) {
            fprintf(f, " on %s", w[t->wait_type]);
            if (t->wait_id) fprintf(f, " 0x%X", t->wait_id);
            if (t->wait_until) fprintf(f, " (%lld us left)", (long long)(t->wait_until - now));
        }
        fprintf(f, "\n");
        rt_shadow_print(&t->shadow, "      ");
    }
}

/* ------------------------------------------------------------------------ */
/* startup */

static Thread *g_root;

extern uint64_t display_frame_count(void);

/* MKU_TIMEOUT=s: dump state and exit after s seconds -- or earlier, as soon
 * as the game has shown no new frame for 15 s (a hang). */
static void *watchdog(void *arg) {
    unsigned limit = (unsigned)(uintptr_t)arg, stalled = 0;
    uint64_t last = 0;
    for (unsigned t = 0; t < limit; t++) {
        sleep(1);
        uint64_t f = display_frame_count();
        stalled = f && f == last ? stalled + 1 : 0;
        last = f;
        if (stalled >= 15) {
            fprintf(stderr, "\n[kernel] HANG: no new frame for 15 s (at %u s, frame %llu)\n", t + 1, (unsigned long long)f);
            break;
        }
    }
    fprintf(stderr, "\n[kernel] MKU_TIMEOUT reached after %lus. Threads:\n", (unsigned long)(uintptr_t)arg);
    {   /* what is running right now: 2000 samples of the last entered function */
        enum { N = 2000 };
        static uint32_t seen[64], hits[64];
        int n = 0;
        for (int i = 0; i < N; i++) {
            uint32_t f = rt_cur_fn;
            int k;
            for (k = 0; k < n && seen[k] != f; k++) {}
            if (k == n && n < 64) { seen[n] = f; hits[n] = 0; n++; }
            if (k < 64) hits[k]++;
            usleep(100);
        }
        fprintf(stderr, "[kernel] running now (%d samples over 0.2 s):\n", N);
        for (int k = 0; k < n; k++)
            if (hits[k] * 100 >= N) fprintf(stderr, "    %5.1f%%  func_%08X\n", 100.0 * hits[k] / N, seen[k] - rt_image_base);
    }
    kernel_dump_threads(stderr);
    rt_hle_report();
    gpu_stats_report();
    gpu_draw_report();
    profile_report();
    proc_dump();
    _exit(0);
}

void kernel_init(void) {
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    g_trace = getenv("MKU_TRACE") != NULL;
    const char *timeout = getenv("MKU_TIMEOUT");
    if (timeout) {
        pthread_t wd;
        pthread_create(&wd, NULL, watchdog, (void *)(uintptr_t)atoi(timeout));
    }
}

Thread *kernel_root_thread(void) {
    pthread_mutex_lock(&g_lock);   /* main() holds the baton while running _start */
    g_root = thread_new("root", 0, 0x20, 0);
    rt_ctx_init(&g_root->ctx);
    g_root->state = TS_RUNNING;
    g_root->host_started = 1;
    g_current = g_root;
    rt_shadow = &g_root->shadow;
    return g_root;
}

void kernel_root_finished(void) {
    thread_finish(g_root, g_root->ctx.r[2]);
    hand_off(g_root);
    pthread_mutex_unlock(&g_lock);   /* the main thread is now free: it runs the window */
}

/* ------------------------------------------------------------------------ */
/* ThreadManForUser: threads */

void hle_sceKernelCreateThread(Ctx *c) {
    const char *name = GSTR(A0);
    uint32_t entry = A1, prio = A2, stack_size = (A3 + 0xFF) & ~0xFFu, attr = T0;
    Thread *t = thread_new(name, entry, prio, attr);
    t->stack_block = mem_alloc(stack_size, 1, 256);
    if (!t->stack_block) RET(ERR_NO_MEMORY);
    t->stack_addr = t->stack_block;
    t->stack_size = stack_size;
    memset(rt_ptr(t->stack_addr), 0xFF, stack_size);   /* the PSP pre-fills stacks too */
    TRACE("(\"%s\", entry=0x%08X, prio=0x%X, stack=0x%X) -> 0x%X", name, entry, prio, stack_size, t->uid);
    RET(t->uid);
}

void hle_sceKernelStartThread(Ctx *c) {
    Thread *t = thread_by_uid((int)A0);
    uint32_t argsize = A1, argp = A2;
    if (!t) RET(ERR_NOT_FOUND_THREAD);
    TRACE("(0x%X \"%s\", argsize=%u)", t->uid, t->name, argsize);

    rt_ctx_init(&t->ctx);
    uint32_t sp = t->stack_addr + t->stack_size;
    if (argp && argsize) {   /* the kernel copies the arguments onto the new stack */
        sp -= (argsize + 15) & ~15u;
        memmove(rt_ptr(sp), rt_ptr(argp), argsize);
        t->ctx.r[5] = sp;
    }
    t->ctx.r[4] = argsize;
    t->ctx.r[29] = sp - 64;
    t->ctx.r[28] = c->r[28];   /* same $gp as the creator */
    t->prio = t->init_prio;
    t->generation++;
    t->host_started = 0;
    t->state = TS_READY;
    t->ready_seq = ++g_seq;
    c->r[2] = 0;
    sched_yield_if_preempted();
}

void hle_sceKernelExitThread(Ctx *c) {
    Thread *t = g_current;
    TRACE("(%d) \"%s\"", (int)A0, t->name);
    thread_finish(t, A0);
    hand_off(t);
    pthread_mutex_unlock(&g_lock);
    pthread_exit(NULL);   /* unwind nothing: this host thread simply ends */
}

void hle_sceKernelGetThreadId(Ctx *c) { RET(g_current->uid); }

void hle_sceKernelDelayThread(Ctx *c) {
    TRACE("(%u us)", A0);
    if (A0 == 0) { kernel_yield(); RET(0); }
    RET(sched_wait(g_current, WAIT_DELAY, 0, A0, 0));
}
/* A delay that callbacks can interrupt: run them, then keep waiting until the deadline. */
void hle_sceKernelDelayThreadCB(Ctx *c) {
    Thread *t = g_current;
    uint64_t until = now_us() + A0;
    kernel_run_callbacks(c);
    for (;;) {
        uint64_t now = now_us();
        if (now >= until) break;
        t->in_cb_wait = 1;
        uint32_t r = sched_wait(t, WAIT_DELAY, 0, until - now, 0);
        t->in_cb_wait = 0;
        if (r != WAKE_FOR_CALLBACK) break;
        kernel_run_callbacks(c);
    }
    RET(0);
}

void hle_sceKernelSleepThreadCB(Ctx *c) {
    Thread *t = g_current;
    TRACE("() \"%s\" wakeups=%d", t->name, t->wakeup_count);
    kernel_run_callbacks(c);
    for (;;) {
        if (t->wakeup_count > 0) { t->wakeup_count--; RET(0); }
        t->in_cb_wait = 1;
        uint32_t r = sched_wait(t, WAIT_SLEEP, 0, 0, 0);
        t->in_cb_wait = 0;
        if (r != WAKE_FOR_CALLBACK) RET(r);
        kernel_run_callbacks(c);
    }
}

void hle_sceKernelWakeupThread(Ctx *c) {
    Thread *t = thread_by_uid((int)A0);
    if (!t) RET(ERR_NOT_FOUND_THREAD);
    TRACE("(0x%X \"%s\")", t->uid, t->name);
    if (t->state == TS_WAITING && t->wait_type == WAIT_SLEEP) sched_wake(t, 0);
    else t->wakeup_count++;
    c->r[2] = 0;
    sched_yield_if_preempted();
}

void hle_sceKernelCancelWakeupThread(Ctx *c) {
    Thread *t = thread_by_uid((int)A0);
    if (!t) RET(ERR_NOT_FOUND_THREAD);
    int n = t->wakeup_count;
    t->wakeup_count = 0;
    RET(n);
}

void hle_sceKernelWaitThreadEnd(Ctx *c) {
    Thread *t = thread_by_uid((int)A0);
    if (!t) RET(ERR_NOT_FOUND_THREAD);
    TRACE("(0x%X \"%s\")", t->uid, t->name);
    if (t->state == TS_DORMANT) RET(t->exit_status);
    uint32_t timeout = A1 ? MEM_LW(A1) : 0;
    RET(sched_wait(g_current, WAIT_THREAD_END, t->uid, timeout, A1));
}

void hle_sceKernelChangeThreadPriority(Ctx *c) {
    Thread *t = thread_by_uid((int)A0);
    if (!t) RET(ERR_NOT_FOUND_THREAD);
    TRACE("(0x%X \"%s\", 0x%X)", t->uid, t->name, A1);
    if (A1) t->prio = A1;
    c->r[2] = 0;
    sched_yield_if_preempted();
}

void hle_sceKernelRotateThreadReadyQueue(Ctx *c) {
    kernel_yield();
    RET(0);
}

void hle_sceKernelTerminateThread(Ctx *c) {
    Thread *t = thread_by_uid((int)A0);
    if (!t || t == g_current) RET(ERR_NOT_FOUND_THREAD);
    TRACE("(0x%X \"%s\")", t->uid, t->name);
    t->generation++;   /* its blocked host thread will never get the baton again */
    t->host_started = 0;
    thread_finish(t, 0x800201AC);   /* "thread terminated" */
    RET(0);
}

void hle_sceKernelDeleteThread(Ctx *c) {
    Thread *t = thread_by_uid((int)A0);
    if (!t || t == g_current) RET(ERR_NOT_FOUND_THREAD);
    TRACE("(0x%X \"%s\")", t->uid, t->name);
    thread_delete(t);
    RET(0);
}

void hle_sceKernelTerminateDeleteThread(Ctx *c) {
    Thread *t = thread_by_uid((int)A0);
    if (!t || t == g_current) RET(ERR_NOT_FOUND_THREAD);
    TRACE("(0x%X \"%s\")", t->uid, t->name);
    thread_finish(t, 0x800201AC);
    thread_delete(t);
    RET(0);
}

void hle_sceKernelSuspendThread(Ctx *c) {
    Thread *t = thread_by_uid((int)A0);
    if (!t || t == g_current) RET(ERR_NOT_FOUND_THREAD);
    TRACE("(0x%X \"%s\")", t->uid, t->name);
    if (t->state == TS_READY) { t->state = TS_WAITING; t->wait_type = WAIT_SUSPEND; t->wait_until = 0; }
    RET(0);
}

void hle_sceKernelResumeThread(Ctx *c) {
    Thread *t = thread_by_uid((int)A0);
    if (!t) RET(ERR_NOT_FOUND_THREAD);
    TRACE("(0x%X \"%s\")", t->uid, t->name);
    if (t->state == TS_WAITING && t->wait_type == WAIT_SUSPEND) sched_wake(t, 0);
    c->r[2] = 0;
    sched_yield_if_preempted();
}

/* Dispatch suspension only matters with real interrupts; report "was enabled". */
void hle_sceKernelSuspendDispatchThread(Ctx *c) { RET(1); }
void hle_sceKernelResumeDispatchThread(Ctx *c) { RET(0); }

void hle_sceKernelReferThreadStatus(Ctx *c) {
    Thread *t = thread_by_uid((int)A0);
    if (!t) RET(ERR_NOT_FOUND_THREAD);
    uint32_t info = A1, size = MEM_LW(info);
    /* SceKernelThreadInfo: size, name[32], attr, status, entry, stack, stackSize,
     * gpReg, initPriority, currentPriority, waitType, waitId, wakeupCount, exitStatus, ... */
    uint32_t status = t->state == TS_RUNNING ? 1 : t->state == TS_READY ? 2 : t->state == TS_WAITING ? 4 : 16;
    uint32_t words[] = {t->attr, status, t->entry, t->stack_addr, t->stack_size, t->ctx.r[28],
                        t->init_prio, t->prio, t->wait_type, (uint32_t)t->wait_id,
                        (uint32_t)t->wakeup_count, t->exit_status};
    memset(rt_ptr(info + 4), 0, size > 4 ? size - 4 : 0);
    memcpy(rt_ptr(info + 4), t->name, size >= 36 ? 32 : 0);
    for (uint32_t i = 0; i < sizeof words / 4 && 36 + 4 * i + 4 <= size; i++) MEM_SW(info + 36 + 4 * i, words[i]);
    RET(0);
}

/* ---- system time: the PSP clock counts microseconds ---- */
void hle_sceKernelGetSystemTimeLow(Ctx *c) { RET(now_us()); }
void hle_sceKernelGetSystemTimeWide(Ctx *c) { RET64(now_us()); }
void hle_sceKernelUSec2SysClock(Ctx *c) {
    MEM_SW(A1, A0);
    MEM_SW(A1 + 4, 0);
    RET(0);
}
void hle_sceKernelSysClock2USecWide(Ctx *c) {
    uint64_t clock = ((uint64_t)A1 << 32) | A0;   /* 64-bit argument in a0:a1 */
    if (A2) MEM_SW(A2, (uint32_t)(clock / 1000000u));
    if (A3) MEM_SW(A3, (uint32_t)(clock % 1000000u));
    RET(0);
}
