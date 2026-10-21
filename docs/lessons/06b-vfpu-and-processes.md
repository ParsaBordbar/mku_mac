# Lesson 06b — The VFPU, and the game's own coroutines

Two very different problems stood between the boot and a running game.

## Part 1: the VFPU

The PSP's vector unit: 128 float registers arranged as 8 matrices of 4x4.
One instruction works on a single value, a 2/3/4-lane vector or a whole
matrix, read along a row or a column.

### Register numbers

A 7-bit register number packs matrix, column and row:

```
bits 2-4  matrix (0-7)      bits 0-1  column      bits 5-6  row / "transposed"
S010 = matrix 0, column 1, row 0 = 1          (a single float)
C010 = column 1 of matrix 0      = 1          (a 4-vector going down)
R001 = row 1 of matrix 0         = 0x21       (a 4-vector going across)
M100 = matrix 1                  = 4,  E100 = matrix 1 transposed = 0x24
```
The same bits mean different things for different vector sizes (a triple
can start at row 0 or 1, a quad only at 0 or 2). We store the registers as
`v[matrix*16 + column*4 + row]` and compute the lanes in `vector_indices()`.

### Prefixes

`vpfxs`/`vpfxt` change how the **next** instruction reads its two inputs,
lane by lane: take another lane (swizzle), take the absolute value, negate,
or use a constant (0, 1, 2, 1/2, 3, 1/3, 1/4, 1/6). `vpfxd` saturates outputs
to [0,1] or [-1,1] and can mask lanes so they aren't written. Every
arithmetic instruction then resets them ("eats" them).

The identity prefix is `0xE4` (lanes 3,2,1,0 = `11 10 01 00`), **not 0**:
a zeroed prefix means "every lane reads x". That's why `rt_ctx_init()` exists.

### How it's executed

The 2,701 VFPU instructions (0.4% of the game) don't get custom C each. The
recompiler emits `vfpu_exec(c, pc, word)` and `runtime/vfpu.c` decodes the
fields at runtime: an interpreter just for the VFPU. Only the VFPU branches
(`bvt`, `bvf`, …) become real `if`/`goto` code, because they have delay slots.

Encodings and semantics follow PPSSPP's `Core/MIPS/MIPSTables.cpp` and
`InterpreterVFPU.cpp`. Documented simplifications (fine for now, worth
revisiting): plain float dot products (the hardware rounds its own way),
`sinf`/`cosf` instead of the PSP's exact tables, no prefixes on matrix ops.

### Testing without an oracle

No assembler or disassembler knows the VFPU, so `tests/recomp/cases/vfpu.c`:
- writes VFPU instructions as `.word` expressions built by C macros
  (`V3(0x64000000, 0x21, 0x20, 0x20, Q)` = `vmul.q R001, R000, R000`),
- on MIPS: C code prepares data, calls the asm helper, hashes the result;
  natively: the same C code calls a plain-C reference instead.

Macro gotcha: `#sz` turns `Q` into the text `"Q"`, not `0x8080`. You need the
two-step `XSTR(x) -> STR(x) -> #x` so the argument expands first.

**Compiler gotcha, found by the tests:** `vsin` was 1 ulp off. The reference
computed `sinf(a)` and `cosf(a)` of the same angle, so clang merged them into
one `__sincosf_stret` call, which rounds differently. Fix: separate,
non-inlined `sin_q`/`cos_q` on both sides. Same family as `-ffp-contract=off`
in lesson 06: **the compiler is allowed to change float results unless you
stop it.**

Mutation-tested: swapped row/column mapping, ignored negate prefix, missing
"any" bit in `vcmp`, swapped `vmmul` operands and an ignored write mask are
all caught.

## Part 2: Midway's process system

After the VFPU, the boot stopped at an unresolved jump in hand-written
assembly around `0x1DC614-0x1DD210`. It's Midway's **process system**, the
cooperative multitasking that Midway arcade games have used since the 80s.
Game objects are "processes": functions that run a bit, **sleep**, and are
resumed next tick by a scheduler.

Read in assembly, `run()` does this:
1. Save the scheduler's `ra`/`sp`.
2. Restore the process's registers and **jump into the middle of a
   function**: the address right after the process's last `sleep()` call.

And `sleep()` saves the registers and **returns past all its callers**,
straight to the scheduler.

Three C++ classes implement it three ways (found via their vtables at
`0x30E9B0`/`0x30E9E0`): no sleeping, stack-copying (processes run on the
main stack and their frames are copied out when they sleep), and private
stacks.

### Why recompiled C can't do this

C can't jump into the middle of a function, and can't return past several
callers. Our recompiled functions call each other with real C calls, so the
C call stack *is* the process's state.

### The fix: fibers + patches

- `runtime/fiber.c`: a 25-line AArch64 context switch. AAPCS64 says only
  x19–x30, sp and d8–d15 survive a call, so a switch pushes those, swaps sp,
  pops the other set (`tests/runtime/fiber_test.c` checks it).
- `runtime/patches/mk_process.c`: C replacements for `run`, `sleep`,
  `chain`, `to/from_main_stack`. Each game process gets its own fiber (its own
  C stack) and its own 64 KiB guest stack. `sleep` = switch to the scheduler;
  `run` = switch back. The whole C call chain of the process simply waits on
  its fiber's stack.
- `config/patches.txt` tells the recompiler which functions to replace,
  the same idea as N64Recomp's patches.

Subtle point: on the PSP the VFPU registers are shared CPU state that the
original switch never saves, so `share_vfpu()` copies them across on every
switch.

## Result

```
[game] SCREAM PSP (c)2004 Sony Computer Entertainment America
[game] file load - ... legal_screen.sec
[display] 2115 frames shown, 29.4 s          ← steady 60 fps, no crash
```
Threads idle normally: `user_main` waits for the next frame, the sound tick
and stream threads run, the movie player gave up (`sceMpeg*` not implemented).
Nothing is drawn yet: that's phase 8.

## Exercises 🧑‍🎓
1. Decode `vmul.q R001, R000, R000` by hand from `0x64000000|(0x20<<16)|(0x20<<8)|0x21|0x8080`
   and check it with `tools/disasm.py` on any `vmul.q` in the game.
2. Why does `vidt.q R003` put the 1 in lane 3? (`vfpu_34`, look at `offmask`.)
3. Count processes: add a `fprintf` in `fiber_for()` and see how many exist
   after 10 s on the legal screen.
4. Remove `share_vfpu()` from `mk_process.c`. Does anything visibly change
   yet? (Probably not until graphics are on: then watch for broken matrices.)
