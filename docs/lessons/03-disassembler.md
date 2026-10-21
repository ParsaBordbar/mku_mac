# Lesson 03 — Reading machine code: the disassembler

`tools/mips.py` turns 32-bit words into instructions. The recompiler will use
the same `decode()`, so every bug found here is a crash avoided later.

## Three layouts cover everything

```
R-type  | op:6 | rs:5 | rt:5 | rd:5 | sa:5 | funct:6 |   addu rd, rs, rt
I-type  | op:6 | rs:5 | rt:5 |        imm:16         |   addiu rt, rs, imm / lw rt, imm(rs) / beq rs, rt, label
J-type  | op:6 |            target:26               |   jal target
```

Worked example — the first instruction of the game, `0x27bdfff0`:

```
0010 01 | 11 101 | 1 1101 | 1111 1111 1111 0000
op=0x09   rs=29    rt=29    imm=0xfff0 → sign-extend → -16
0x09 = addiu, register 29 = $sp      →  addiu $sp, $sp, -16
```
"Make 16 bytes of room on the stack" — every function starts like this.

## What the game uses

```
$ python3 -I tools/disasm.py BOOT.elf --stats
708048 instructions, 100 distinct mnemonics, invalid: 0
lw 122698 · sw 64693 · addiu 57745 · sll 57149 (mostly nop) · or 55947 (mostly move) ...
```

A real game is dominated by loads, stores, moves and calls. Floating point
(`lwc1`, `mul.s`, …) is ~10% — it's a 3D fighting game. VFPU is only 0.4%.

## Pseudo-instructions

Assemblers print friendlier names for common patterns. They are *not* separate
instructions — just aliases, and the recompiler only sees the canonical one:

| Printed | Really is |
|---|---|
| `nop` | `sll $zero, $zero, 0` (word `0x00000000`) |
| `move a, b` | `addu a, b, $zero` or `or a, b, $zero` |
| `b label` | `beq $zero, $zero, label` |
| `beqz a, label` | `beq a, $zero, label` |
| `negu a, b` | `subu a, $zero, b` |
| `bal label` | `bgezal $zero, label` |

## Allegrex oddities

The PSP CPU is "MIPS II + bits". Things a standard MIPS tool gets wrong:
- `clz`/`clo`, `madd`/`msub` live in the **SPECIAL** table (funct 0x16/0x17,
  0x1C–0x2F); on MIPS32 they're in SPECIAL2.
- `max`/`min` (SPECIAL 0x2C/0x2D) don't exist on any other MIPS.
- `ext`, `ins`, `seb`, `seh`, `wsbh`, `rotr` come from MIPS32r2.
- Only single-precision floats (`.s`), no doubles in hardware.
- The **VFPU** (opcodes 0x18, 0x19, 0x1B, 0x34, 0x36, 0x37, 0x3C–0x3F, plus
  COP2 0x12) — a 128-register vector unit. Phase 6.

## Reading the entry point

```
_start:
  1dd22c: jal  sceKernelSetCompiledSdkVersion   ; a0 = 0x02070010 → SDK 2.7.0
  1dd238: jal  sceKernelSetCompilerVersion      ; a0 = 0x00030306 → GCC 3.3.6 (probably)
  1dd240: lui  $a0, 0x0 / addiu $v0, $a0, 0 / beqz $v0, ...
          ; address of an optional "weak" symbol; it's 0 = not linked → skip
  1dd294: lui $t8, 0x1e / addiu $a1, $t8, -11492  ; a1 = 0x1dd31c (thread entry)
  1dd29c: jal  sceKernelCreateThread
  1dd2ac: jal  sceKernelStartThread
```

Note how the argument registers are set in the **delay slot** right after
`jal` (`ori $a0, $v0, 0x306`). Read MIPS with that in mind, always.

The compiler version is gold for later: a byte-matching decompilation needs
the exact compiler. GCC 3.3.6-based Sony PSP toolchain is the first guess.

## The oracle test

```
$ python3 -I tools/check_disasm.py BOOT.elf
```
Compares every instruction with LLVM's MIPS disassembler (on the *relocated*
code so both see the same bytes). PSP-only instructions are counted
separately since LLVM doesn't know them.

How it went — a typical oracle story:

| Run | Match | What was wrong |
|---|---|---|
| 1 | 23.7% | Only *style*: LLVM prints `$4` not `$a0`, `0x10` not `16`. Added `text(llvm=True)`. |
| 2 | 99.68% | Aliases `beqzl`/`bnezl`; LLVM prints `div $zero, a, b`; `break` has **two** code fields; LLVM prints runs of zeros as `...`. |
| 3 | 99.9996% | `break` with both codes 0 prints bare. |
| 4 | **100%** (705,309 / 705,309) | — |

None of these were decoding bugs, but run 2 taught something real: GCC
emits `break 0, 7` after a division to trap divide-by-zero. The recompiler
will see those and must not crash on them.

The 2,739 PSP-only instructions (VFPU, `min`/`max`, `clz`, `bitrev`) can't be
checked by LLVM. They'll get their own tests in phase 6.

## Exercises 🧑‍🎓
1. Decode `0x0c0acf32` by hand (it's at `0x1dd22c`). Check the target with
   `disasm.py`. Why is the top nibble of the target taken from `addr + 4`?
2. `_start` returns `v0 = 0`. Find what the main thread function (`0x1dd31c`)
   calls first: `python3 -I tools/disasm.py BOOT.elf 0x1dd31c`.
3. In `mips.py`, `andi`/`ori`/`xori` zero-extend their immediate but `addiu`
   sign-extends it. Find the line that handles it. What would go wrong in the
   recompiler if `ori` sign-extended?
4. Find a `beql` in the game. Explain in one sentence what happens to its delay
   slot when the branch is *not* taken.
