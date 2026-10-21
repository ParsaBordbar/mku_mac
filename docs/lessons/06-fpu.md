# Lesson 06 — Floating point

The PSP's FPU is IEEE-754 single precision, exactly like a C `float` on the
Mac. So most instructions are one line each (`tools/recomp.py`, `fpu()`):

```c
c->f[0] = c->f[12] * c->f[13];        /* mul.s $f0, $f12, $f13 */
c->fi[2] = MEM_LW(c->r[4] + 8u);       /* lwc1  $f2, 8($a0)  -- raw bits, no conversion */
c->fcc = FCMP(c->f[0], c->f[2], 4);    /* c.olt.s $f0, $f2  */
```

`Ctx` has a union: `f[]` views a register as a float, `fi[]` as its raw 32 bits.
Loads, stores, `mov.s`, `mtc1`/`mfc1` copy bits; they never convert.

## Subtleties that matter

| Issue | Why | What we do |
|---|---|---|
| **No fused multiply-add on PSP** | clang on ARM64 happily turns `a*b + c` into one `fmadd` with one rounding. The PSP rounds twice. | Compile everything with `-ffp-contract=off` (Makefile and tests). |
| **Compare + branch** | `c.cond.s` sets a flag, `bc1t`/`bc1f` branch on it later. | `c->fcc`; the low 4 bits of the instruction say which of unordered / equal / less make it true. |
| **NaN** | Any comparison with NaN is "unordered". | `FCMP` checks it first. |
| **float → int** | Out-of-range or NaN is undefined behaviour in C. MIPS gives 0x7FFFFFFF. | `CVT_W_S` checks range first, then applies the rounding mode. |
| **`neg.s` / `abs.s`** | Only flip/clear the sign bit. | Done on `fi[]` bits, so even NaN payloads survive. |

## Testing it: the NaN lesson

`tests/recomp/cases/float.c` feeds NaN, ±infinity, -0.0 and denormals through
arithmetic, comparisons, conversions and calls. The first run failed one test:
both sides returned NaN, but **different NaNs**. When two NaNs meet, which
one's "payload" bits survive depends on operand order, and the native build's
optimizer had swapped operands of a commutative `+`. MIPS and ARM don't even
agree on which bit marks a "quiet" NaN. So arithmetic results are compared by
NaN-ness (`arith_bits`); bit-copying instructions are still compared exactly.

Mutation check: a NaN-insensitive compare, `trunc.w.s` rounding to nearest,
and swapped `sub.s` operands were all caught.

## Exercise 🧑‍🎓
Remove `-ffp-contract=off` from `tests/recomp/run.py` (both places), run
`make test`, and explain the failure you get. Then put it back.
