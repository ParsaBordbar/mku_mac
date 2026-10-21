# Lesson 02 — Finding every function

Machine code is one long stream of 4-byte instructions. Nothing marks where a
function starts. A recompiler works function by function, so we need the list.

```
$ python3 -I tools/find_functions.py BOOT.elf -o build/functions.csv
functions found : 13167  (+249 import stubs)
  with jal       : 8622     ← someone calls it
  with data_ptr  : 3247     ← pointer in .data (vtables, callback tables)
  with code_ptr  : 1527     ← address built in code (lui+addiu), e.g. thread entry
maybe-functions : 2627
```

## Evidence, strongest first

| Source | Why it means "function" |
|---|---|
| `jal X` | `jal` = "jump and link" = call. X is a function. |
| pointer in `.data` | C++ vtables, arrays of callbacks. |
| `lui`+`addiu` building a code address | passing a function pointer, e.g. `sceKernelCreateThread("main", func_0001D000, ...)` |
| pointer in `.rodata` | **ambiguous**: const function table *or* a `switch` jump table (case labels are *inside* a function). 2,187 of these. |
| code after `jr $ra` | `jr $ra` = return. Code after it (skipping padding) *might* be a new function nobody references. 440 of these. |

Relocations make the first three exact. Without them we'd scan for `jal`-looking
bit patterns and also hit random data that happens to look like `jal`.

## How we checked it

A real function start almost always follows the end of the previous function
(`jr $ra` + its delay slot + maybe `nop` padding), or a tail-call `j`:

```
jal        8606/8621 = 99.8%
data_ptr   3244/3247 = 99.9%
code_ptr   1474/1527 = 96.5%   ← suspicious
```

**This is the "oracle" habit:** every tool's output gets an independent check.

## Branch delay slots (you'll meet them everywhere)

On MIPS the instruction *after* a jump runs **before** the jump lands:

```
jal   func_00012340
move  a0, s0        ← runs first! this sets the argument
```

That's why a function's last instruction is not `jr $ra` but the one after it.

## Exercises 🧑‍🎓
1. **The 53 suspicious `code_ptr`s.** List them (`grep code_ptr build/functions.csv`
   then check the instruction before each). Are they (a) functions after a
   `jr` to another register, `break`, or an infinite loop; (b) wrong HI16/LO16
   pairing in `find_functions.py` (it pairs each LO16 with the *latest* HI16 —
   when is that wrong?); or (c) code addresses that aren't function starts?
   Fix the tool if (b).
2. **The 440 `after_ret` blocks.** Pick 5, disassemble them with Homebrew LLVM
   (Apple's `llvm-objdump` has no MIPS support; `brew install llvm` first):
   `$(brew --prefix llvm)/bin/llvm-objdump -d --triple=mipsel -mcpu=mips2 --start-address=0x... --stop-address=0x... BOOT.elf`.
   Real functions? Unused code the linker kept? Should the recompiler include them?
   (Hint: including unused code is harmless; missing reachable code crashes.)
3. **Switch tables.** Find one `rodata_ptr` group (consecutive addresses in
   `build/functions_maybe.csv`), then find the `jr` that uses it. This is the
   pattern Phase 5 must recognize.
