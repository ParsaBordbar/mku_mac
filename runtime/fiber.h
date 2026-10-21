/*
 * Fibers: cooperative threads in one OS thread. A fiber is just a separate
 * host stack plus the registers needed to resume it; fiber_switch() saves the
 * current callee-saved registers on the current stack, swaps the stack
 * pointer, and restores the other fiber's registers.
 *
 * Used for the game's own coroutine "processes" (patches/mk_process.c).
 */
#pragma once

#include <stddef.h>

typedef struct Fiber {
    void *sp;   /* saved host stack pointer while not running */
} Fiber;

/* Prepare `f` to run fn(arg) on the given stack the first time it is switched to.
 * fn must never return; it ends by switching away for good. */
void fiber_init(Fiber *f, void *stack, size_t size, void (*fn)(void *), void *arg);

/* Save the running context into `from` and continue `to`. Returns when
 * something switches back to `from`. */
void fiber_switch(Fiber *from, Fiber *to);
