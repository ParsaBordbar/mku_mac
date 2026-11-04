/* Fiber switch for AArch64 (Apple Silicon Macs, Nintendo Switch, ARM Linux)
 * and x86-64 (Intel Macs, Linux PCs: System V calling convention).
 *
 * The AAPCS64 calling convention says a called function must preserve
 * x19-x28, x29 (frame pointer), x30 (link register), sp and d8-d15; System V
 * x86-64 says rbx, rbp, r12-r15 and rsp. So a switch only needs to push those,
 * swap the stack pointer, and pop the other fiber's. */
#include "fiber.h"

#include <stdint.h>
#include <string.h>

#ifdef __APPLE__
#define SYM(name) "_" #name
#else
#define SYM(name) #name
#endif

#if defined(__aarch64__)

#define FRAME 160   /* 10 GPR pairs/FPR pairs * 16 bytes */

__asm__(
    ".text\n"
    ".globl " SYM(fiber_switch) "\n"
    ".p2align 2\n"
    SYM(fiber_switch) ":\n"           /* x0 = from, x1 = to */
    "    sub  sp, sp, #160\n"
    "    stp  x19, x20, [sp, #0]\n"
    "    stp  x21, x22, [sp, #16]\n"
    "    stp  x23, x24, [sp, #32]\n"
    "    stp  x25, x26, [sp, #48]\n"
    "    stp  x27, x28, [sp, #64]\n"
    "    stp  x29, x30, [sp, #80]\n"
    "    stp  d8,  d9,  [sp, #96]\n"
    "    stp  d10, d11, [sp, #112]\n"
    "    stp  d12, d13, [sp, #128]\n"
    "    stp  d14, d15, [sp, #144]\n"
    "    mov  x9, sp\n"
    "    str  x9, [x0]\n"             /* from->sp = sp */
    "    ldr  x9, [x1]\n"
    "    mov  sp, x9\n"               /* sp = to->sp */
    "    ldp  x19, x20, [sp, #0]\n"
    "    ldp  x21, x22, [sp, #16]\n"
    "    ldp  x23, x24, [sp, #32]\n"
    "    ldp  x25, x26, [sp, #48]\n"
    "    ldp  x27, x28, [sp, #64]\n"
    "    ldp  x29, x30, [sp, #80]\n"
    "    ldp  d8,  d9,  [sp, #96]\n"
    "    ldp  d10, d11, [sp, #112]\n"
    "    ldp  d12, d13, [sp, #128]\n"
    "    ldp  d14, d15, [sp, #144]\n"
    "    add  sp, sp, #160\n"
    "    ret\n"                       /* to x30: where `to` called fiber_switch, or the trampoline */

    ".p2align 2\n"
    SYM(fiber_trampoline) ":\n"       /* first run of a fiber: x19 = fn, x20 = arg */
    "    mov  x0, x20\n"
    "    blr  x19\n"
    "    brk  #0\n");                 /* fn must not return */

extern void fiber_trampoline(void);

void fiber_init(Fiber *f, void *stack, size_t size, void (*fn)(void *), void *arg) {
    uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
    uint64_t *frame = (uint64_t *)(top - FRAME);
    memset(frame, 0, FRAME);
    frame[0] = (uint64_t)(uintptr_t)fn;                 /* x19 */
    frame[1] = (uint64_t)(uintptr_t)arg;                /* x20 */
    frame[11] = (uint64_t)(uintptr_t)fiber_trampoline;  /* x30: "return" into the trampoline */
    f->sp = frame;
}

#elif defined(__x86_64__)

__asm__(
    ".text\n"
    ".globl " SYM(fiber_switch) "\n"
    ".p2align 4\n"
    SYM(fiber_switch) ":\n"           /* rdi = from, rsi = to */
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rsp, (%rdi)\n"         /* from->sp = rsp */
    "    movq (%rsi), %rsp\n"         /* rsp = to->sp */
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"

    ".p2align 4\n"
    SYM(fiber_trampoline) ":\n"       /* first run of a fiber: r12 = fn, r13 = arg */
    "    movq %r13, %rdi\n"
    "    call *%r12\n"
    "    ud2\n");                     /* fn must not return */

extern void fiber_trampoline(void);

void fiber_init(Fiber *f, void *stack, size_t size, void (*fn)(void *), void *arg) {
    /* r15 r14 r13 r12 rbx rbp, then the return address. After the ret, rsp must
     * be 16-byte aligned so that the trampoline's call meets the ABI. */
    uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
    uint64_t *frame = (uint64_t *)(top - 72);
    memset(frame, 0, 72);
    frame[2] = (uint64_t)(uintptr_t)arg;               /* r13 */
    frame[3] = (uint64_t)(uintptr_t)fn;                /* r12 */
    frame[6] = (uint64_t)(uintptr_t)fiber_trampoline;  /* "return" into the trampoline */
    f->sp = frame;
}

#else
#error "fiber.c: this CPU isn't supported yet (AArch64 and x86-64 are)"
#endif
