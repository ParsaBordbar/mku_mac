/*
 * HLE ("high-level emulation") of the PSP kernel: instead of running Sony's
 * firmware, we reimplement each function the game imports, in C.
 *
 * Calling convention: arguments arrive in the guest registers. The PSP uses
 * an EABI-like convention with EIGHT integer argument registers:
 *     a0-a3 = r4-r7, then t0-t3 = r8-r11. Return value in v0 (r2), 64-bit in v0:v1.
 * A 64-bit argument takes an aligned register pair (e.g. a2:a3).
 */
#pragma once

#include "recomp.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A0 (c->r[4])
#define A1 (c->r[5])
#define A2 (c->r[6])
#define A3 (c->r[7])
#define T0 (c->r[8])
#define T1 (c->r[9])
#define RET(v) do { c->r[2] = (uint32_t)(v); return; } while (0)
#define RET64(v) do { uint64_t r_ = (uint64_t)(v); c->r[2] = (uint32_t)r_; c->r[3] = (uint32_t)(r_ >> 32); return; } while (0)

/* Guest strings: a NUL-terminated string at a guest address. */
static inline const char *GSTR(uint32_t addr) { return addr ? (const char *)rt_ptr(addr) : "(null)"; }

/* Logging. MKU_TRACE=1 prints every implemented HLE call too. */
extern int g_trace;
/* Inside hle_* functions the name is printed without "hle_", plus the caller's
 * return address (where in the game the call came from). */
#define TRACE(fmt, ...) \
    do { if (g_trace) fprintf(stderr, "[hle] %s" fmt "  <- 0x%08X\n", strncmp(__func__, "hle_", 4) ? __func__ : __func__ + 4, ##__VA_ARGS__, c->r[31]); } while (0)
#define TRACE_NOCTX(fmt, ...) \
    do { if (g_trace) fprintf(stderr, "[hle] %s" fmt "\n", __func__, ##__VA_ARGS__); } while (0)
#define LOG(fmt, ...) fprintf(stderr, "[hle] %s" fmt "\n", __func__ + 4, ##__VA_ARGS__)

/* Kernel error codes (subset). Values as used by PPSSPP's Core/HLE/ErrorCodes.h.
 * TODO: double-check each against that file when its behaviour matters. */
#define ERR_NOT_FOUND_THREAD    0x80020198u
#define ERR_NOT_FOUND_SEMA      0x80020199u
#define ERR_NOT_FOUND_EVF       0x8002019Au
#define ERR_NOT_FOUND_FPL       0x8002019Cu
#define ERR_NOT_FOUND_CALLBACK  0x800201A1u
#define ERR_WAIT_TIMEOUT        0x800201A8u
#define ERR_SEMA_ZERO           0x800201ADu
#define ERR_SEMA_OVERFLOW       0x800201AEu
#define ERR_EVF_COND            0x800201AFu
#define ERR_WAIT_DELETE         0x800201B5u
#define ERR_NO_MEMORY           0x80020190u
#define ERR_ILLEGAL_ARGUMENT    0x800200D2u
#define ERR_FILE_NOT_FOUND      0x80010002u

/* ---- object ids ("UIDs") ---- */
enum ObjType { OBJ_NONE, OBJ_THREAD, OBJ_SEMA, OBJ_EVF, OBJ_FPL, OBJ_CALLBACK, OBJ_MEMBLOCK, OBJ_MODULE };
int kobj_new(enum ObjType type, void *ptr);
void *kobj_get(int uid, enum ObjType type);
void kobj_free(int uid);

/* ---- threads and scheduling (sched.c) ---- */
enum WaitType { WAIT_NONE, WAIT_DELAY, WAIT_SLEEP, WAIT_SEMA, WAIT_EVF, WAIT_THREAD_END, WAIT_SUSPEND };
enum ThreadState { TS_DORMANT, TS_READY, TS_RUNNING, TS_WAITING, TS_DELETED };

typedef struct Thread {
    int uid;
    char name[32];
    uint32_t entry, init_prio, prio, attr, stack_addr, stack_size, stack_block;
    enum ThreadState state;
    Ctx ctx;
    /* waiting */
    enum WaitType wait_type;
    int wait_id;
    uint64_t wait_until;        /* host microseconds; 0 = no timeout */
    uint32_t wait_arg[3];       /* meaning depends on wait_type */
    uint32_t timeout_addr;      /* guest pointer updated with remaining time, or 0 */
    uint32_t wait_result;       /* v0 the wait returns with */
    int wakeup_count;
    int in_cb_wait;             /* waiting in a *CB function: callbacks may wake it */
    uint32_t exit_status;
    uint64_t ready_seq;         /* round robin among equal priorities */
    /* host side */
    RtShadow shadow;            /* recompiled call chain (debugging) */
    int generation;             /* bumps on restart; stale host threads ignore the baton */
    int host_started;
    pthread_t host;
    pthread_cond_t cv;
} Thread;

extern Thread *g_current;
uint64_t now_us(void);

void kernel_init(void);
Thread *kernel_root_thread(void);         /* the thread main() runs _start on */
void kernel_root_finished(void);          /* _start returned: hand over to the game's threads */

/* Block the current thread (fields wait_* already set) until something wakes it.
 * Returns the thread's wait_result. */
uint32_t sched_wait(Thread *t, enum WaitType type, int id, uint64_t timeout_us, uint32_t timeout_addr);
void sched_wake(Thread *t, uint32_t result);       /* make a waiting thread ready */
void sched_yield_if_preempted(void);               /* after waking someone: switch if they outrank us */
void kernel_yield(void);                            /* rotate among equal priority */
Thread *thread_by_uid(int uid);                    /* uid 0 = current thread */
void kernel_dump_threads(FILE *f);
void kernel_run_interrupts(void);                  /* vblank handlers (display.c) */

/* callbacks (sync.c) */
#define WAKE_FOR_CALLBACK 0xC0FFEEu                 /* wait result: woken to run a callback */
void kernel_notify_callback(int uid, uint32_t arg);
int kernel_run_callbacks(Ctx *c);                   /* run the current thread's notified callbacks */

/* ---- user memory (sysmem.c) ---- */
void sysmem_init(void);
uint32_t mem_alloc(uint32_t size, int from_top, uint32_t align);
void mem_free(uint32_t addr);
uint32_t mem_free_total(void);
uint32_t mem_free_max(void);

/* ---- vblank (display.c) ---- */
#define VBLANK_US 16683u
uint64_t vblank_count(void);
uint64_t next_vblank_us(void);
