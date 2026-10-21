# Lesson 07 — Reimplementing the PSP kernel (HLE)

The game talks to the outside world only through its 249 imports. "HLE"
(high-level emulation) means we write those functions in C ourselves instead of
running Sony's firmware. `runtime/hle/`:

| File | Imports it covers |
|---|---|
| `sched.c` | threads, the scheduler, system time |
| `sync.c` | semaphores, event flags, fixed pools, callbacks |
| `sysmem.c` | memory allocation, `sceKernelPrintf`, cache/interrupt toggles |
| `io.c` | files: disc (straight from the ISO), memory stick, stdout |
| `display.c` | display, vblank interrupts, graphics engine (stub), controller |
| `system.c` | UMD, power, settings, module loading, audio (silent, paced) |

`build/recomp/hle_stubs.c` still has a `weak` default for every import. Writing
a function with the same name anywhere in `runtime/hle/` replaces it, so
unimplemented calls keep logging and the list shrinks as you go.

## Calling convention

```c
void hle_sceIoOpen(Ctx *c) {             /* int sceIoOpen(const char *path, int flags, int mode) */
    int fd = do_open(GSTR(A0), A1);      /* A0..A3 = $a0..$a3, then T0..T3 = $t0..$t3 */
    RET(fd);                             /* $v0 */
}
```
The PSP passes **8** integer arguments in registers (a0–a3, t0–t3). A 64-bit
argument takes an aligned pair, e.g. `sceIoLseek(fd, offset64, whence)` gets
the offset in a2:a3 and `whence` in t0.

## Threads: one baton

The PSP has one CPU core. The highest-priority ready thread runs until it
blocks. We give each PSP thread a host thread, but only the one holding the
**baton** (`g_lock` + `g_current`) may run game code:

```
user_main ──WaitSema──► blocks ──► scheduler picks UMDAReadManager ──► signal its cv
                                                                      │
UMDAReadManager wakes, runs game code ... SleepThread ──► picks user_main again
```

Every switch happens inside an HLE call, at a point we control, so the game
stays exactly as single-threaded as it was. When nothing is ready, the
scheduler sleeps until the next timeout or vblank. After 5 s of that it
prints every thread and what it waits on, which is how you debug a hang.

`MKU_TRACE=1` logs every HLE call; `MKU_TIMEOUT=20` stops after 20 s and
prints the thread table plus the unimplemented-call summary.

## Debugging story 1: 130 bad jumps → 3

The first boot died at `[rt] unresolved jump at 0x08A4D2C0 -> 0x08A4D7C4`.
The target lay inside `func_002495FC`, a "function" found only via `code_ptr`.
Tracing which `lui`/`addiu` pair produced it:

```
lui   $a2, 0x25             ← HI16
addiu $a2, $a2, -28156      ← its real LO16
sw    $a2, 268($a0)
lw    $a0, -27140($a1)      ← ALSO a LO16 -- for an earlier `lui $a1, 0x3b`!
```

GCC loads `lui $a1, 0x3b` once and reuses it for many loads. `find_functions.py`
paired every LO16 with the *latest* HI16, so `0x250000 - 27140 = 0x2495FC`
looked like a code pointer. Fix: pair a LO16 with the last `lui` into **the
register it reads**, and only count `addiu`/`ori` (a `lw` reads data, it
doesn't build a function pointer). Result: exactly the **53** suspicious
starts from lesson 02 disappeared, and `bad_jump` dropped from 130 to 3.

(The loader's relocation code was fine: it pairs each HI16 with the *next*
LO16 like the PSP's own loader, and the linker always puts the real partner
right after.)

## Debugging story 2: the disc as a block device

Next crash: a C++ virtual call through a NULL vtable entry. The trace showed:

```
sceIoOpen("umd0:")                                   ← the raw disc, no file name
sceIoGetstat("disc0:/PSP_GAME/USRDIR/ARCHIVE.BIN")  ← "where is this file?"
sceIoLseek("umd0:", 143535) ; sceIoRead(..., 1)      ← sectors, not bytes
```

On a PSP, `sceIoGetstat` puts a disc file's first sector (LBN) in
`st_private[0]`, and the game reads that sector range straight from `umd0:`.
We returned 0, so it read garbage. The fix: `io.c` now reads the **ISO file
itself** with a ~40-line ISO9660 parser (`iso_lookup`), reports real LBNs,
and serves `umd0:` in 2048-byte sectors. Bonus: no more mounting the ISO.
The game's own log confirms it: `UMD SEEK from 0 to 143536`.

## Where it stops now

```
sceDisplaySetMode(0, 480, 272)
[ge] first display list at 0x08B83200
[display] first framebuffer: ...
[game] 32bit - A=8 R=8 G=8 B=8 Mode set...
[rt] unimplemented instruction vfpu4 (0xD003809C) at 0x08A9DE1C
```

Graphics initialised, then the first VFPU instruction: phase 6b (lesson 06b), after which the game runs at 60 fps.

## Exercises 🧑‍🎓
1. Run `MKU_TRACE=1 make run` and draw the thread timeline for the first
   second: which threads exist, which priorities, who wakes whom?
2. The semaphores print as `"(null)"`. Find out why (hint: look at what
   `sceKernelCreateSema` receives in `A0`, and what `GSTR(0)` returns).
3. `sceIoDevctl` is still a stub. Add a TRACE of its `indata`/`outdata`
   pointers and lengths, and look up the command numbers it gets.
