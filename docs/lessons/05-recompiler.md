# Lesson 05 — The static recompiler

```
$ make test   # 12/12 test programs pass
$ make        # whole game → C → native arm64 binary (≈3 s recompile, ≈11 s compile)
$ make run
[main] image 0x08804000-0x08DDAFC0, calling _start at 0x089E1210
[hle] UNIMPLEMENTED sceKernelSetCompiledSdkVersion(0x2070010, ...)
[hle] UNIMPLEMENTED sceKernelSetCompilerVersion(0x30306, ...)
[hle] UNIMPLEMENTED sceKernelCreateThread(0x8AD4F58, 0x89E131C, 0x20, 0x80000)
[hle] UNIMPLEMENTED sceKernelStartThread(0x0, 0x21, 0x9FFF100, 0x80000)
[main] _start returned v0=0x0
```

That is the game's real startup code running as native ARM64 on a Mac.
No emulator: no instruction fetching, no decoding at runtime.

## The pieces

| File | Role |
|---|---|
| `tools/program.py` | What the recompiler needs: memory image, functions, imports, switch-case labels |
| `tools/recomp.py` | MIPS → C, one C function per MIPS function |
| `runtime/recomp.h` | `Ctx` (the register file), memory access, ALU helpers |
| `runtime/runtime.c` | Memory reservation, indirect calls, error reporting |
| `runtime/main_game.c` | `main()`: load the image, set up stack + argv, call `_start` |
| `build/recomp/hle_stubs.c` | One `weak` stub per PSP import: log + return 0 |
| `tests/recomp/` | The test suite |

## One instruction → one statement

```c
c->r[29] = c->r[29] - 16u;            /* addiu $sp, $sp, -16 */
MEM_SW(c->r[29] + 4u, c->r[17]);      /* sw    $s1, 4($sp)   */
c->r[2] = 0x30000u;                   /* lui   $v0, 0x3      */
```

Registers live in `Ctx` because a recompiled function must hand the exact CPU
state to the next one. clang still optimizes a lot: inside a function it keeps
`c->r[...]` in host registers between memory accesses.

### Memory: one AND and one ADD
The PSP's addresses are mirrored (`0x08000000` RAM = `0x48000000` uncached).
`rt_ptr(addr) = rt_mem + (addr & 0x3FFFFFFF)` folds all mirrors into one
1 GiB host reservation. The OS only commits pages that are touched.

### Where the game lives
`program.from_psp()` relocates the module to `0x08804000`, the address a real
PSP uses for game modules. Any base would work (we have the relocations), but
matching the real one means addresses in our logs match PPSSPP's logs later.

## Control flow — the hard part

```c
/* bne $v0, $t0, L ; delay: or $a1, $at, $zero */
{ int cond = c->r[2] != c->r[8]; c->r[5] = c->r[1]; if (cond) goto L_08804020; }
```

1. **Condition first, then delay slot, then jump.** The delay slot may
   overwrite a register the condition reads (GCC does this in loops).
2. **Branch-likely** (`beql`, `bnel`, 1,692 in the game): the delay slot runs
   *only if taken*: `if (cond) { delay; goto L; }`.
3. **`jal` writes `$ra` before the delay slot runs**, so the delay slot sees
   the new value: `c->r[31] = ret; delay; rc_f(c);`.
4. **Calls are C calls.** `jal f` → `rc_f(c)`, and `jr $ra` → `return`. This
   assumes functions return to their caller, which compiled C always does.
5. **Jump tables:** `jr $t0` → `switch (t0) { case <every case label in this function>: goto ...; default: indirect call }`.
6. **Function pointers:** `jalr` → `rt_call_indirect(c, addr)`, a binary
   search in the generated address→function table.
7. **A delay slot that is itself a jump target** is emitted twice: inside the
   branch, and standalone behind a label (the fall-through path skips it).

## How we know it's right: differential testing

`tests/recomp/run.py` builds each test program **twice**:

```
cases/arith.c ──clang (host)────────────────────────────────► native t_add_sub()
             └─clang --target=mipsel ─► ld.lld ─► recomp.py ─► rc_t_add_sub()
```

and calls both 200 times with the same inputs (edge values like `0x80000000`
mixed with random ones). Any difference is a recompiler bug. Each program is
built at `-O0` and `-O2` because they produce very different code.

### Testing the tests: mutation testing
A test suite that passes on its first run proves nothing until you check that
it **can** fail. We broke the recompiler on purpose:

| Deliberate bug | Caught? |
|---|---|
| `subu` emitted as addition | ✅ 10/10 programs fail |
| `sra` emitted as `srl` (no sign) | ✅ 4/10 |
| `lb` zero-extends | ✅ 3/10 |
| Branch condition evaluated **after** the delay slot | ❌ **not caught** |

clang never puts an instruction in a delay slot that changes the branch's own
register, but GCC (which built the game) does. So `cases/asm.c` was added with
hand-written assembly for exactly those patterns. After that:

| Deliberate bug | Caught by |
|---|---|
| Condition after delay slot | ✅ `t_delay_cond` |
| `$ra` set after the delay slot | ✅ `t_jal_ra` |
| `beql` treated as `beq` | ✅ `t_likely` |

`asm.c` also covers the PSP-only `max`/`min`/`clz` (as raw `.word`s, since no
assembler knows them) and the MIPS32r2 `ext`/`ins`/`seb`/`seh`/`wsbh`/`rotr`.

## What's not done (the statistics `recomp.py` prints)

| Item | Count | Phase |
|---|---|---|
| FPU instructions (`lwc1`, `mul.s`, `c.lt.s`, …) | ~89,000 | 6 ✅ |
| VFPU instructions | ~2,700 | 6 |
| `bad_jump`: a branch leaves its function | 130 → 3 | fixed in phase 7 |
| HLE stubs that only log | 249 | 7 (most done) |

## Exercises 🧑‍🎓
1. ~~Fix the false function starts.~~ This became the boot blocker in phase 7
   and is solved there (lesson 07, "Debugging story 1"). New exercise: look
   at the **3** `bad_jump`s and **5** `call_unknown`s that remain
   (`grep -n rt_bad_jump build/recomp/funcs_*.c`). What are they?
2. **Read your own output.** Find `rc_func_001DD31C` (the main thread) in
   `build/recomp/funcs_*.c` and match the first 10 lines against
   `tools/disasm.py BOOT.elf 0x1dd31c`.
3. **Write a mutation.** Break `sltiu` so it doesn't sign-extend its
   immediate. Does the suite catch it? If not, add a case to `arith.c` that does.
4. **Read the thread name.** `sceKernelCreateThread`'s first argument is a
   pointer to the thread's name. Print the string at `0x08AD4F58` from the
   image (hint: `program.from_psp(...).memory[addr - base:]`).
