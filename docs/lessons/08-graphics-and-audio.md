# Lesson 08: from a black window to a playable game

Phases 8 to 10 in one lesson: the GE renderer, the sound engines, input, saves,
and the bugs that stood between "it runs" and "you can finish a fight".

## The GE is a command processor

The game never touches pixels. It writes **display lists** (arrays of 32-bit
commands, top 8 bits = command, low 24 = data) and hands them to
`sceGeListEnQueue`. `runtime/gpu/ge.c` walks a list and records each value in
`ge.cmd[]`, like the chip's registers. Only `PRIM` draws, using whatever the
registers say at that moment. `runtime/gpu/raster.c` turns a `PRIM` into pixels
in emulated VRAM:

    PRIM -> decode vertices -> skinning, lighting, transform -> clip
         -> triangles/sprites -> per pixel: texture, alpha test, fog,
            stencil, depth, blend, colour mask -> framebuffer

## Why a software renderer

A GPU version needs framebuffer tracking, shader generation and readback before
it shows anything. A software renderer can be finished, tested pixel by pixel
and read in one sitting. It is also an oracle for a future GPU backend.

## Sound: three layers

| Layer | File | What it does |
|---|---|---|
| Output | `runtime/host/audio.c` | Mixes the PSP's 8 channels into SDL audio |
| `sceSasCore` | `runtime/hle/sas.c` | Effects: VAG ADPCM, ADSR envelopes, pitch |
| `sceAtrac3plus` | `runtime/hle/atrac.c` | `.MUS` music: FFmpeg's ATRAC3 decoder, streaming, loops |

The bug worth remembering: the loop offsets in the `smpl` chunk were 8 bytes off,
so the "loop" was 0 to 0 and the music was one 23 ms fragment repeated. It was
found by checking the captured audio (`MKU_WAV=1`) instead of listening.

## The hang after the legal screen had three causes

1. A memory stick callback was never fired: real callbacks and devctls fixed it.
2. `main()` changes game mode with `longjmp`, recompiled as a plain return. See
   `RT_SETJMP` and `rt_longjmp`.
3. The save-data utility was missing: `runtime/hle/savedata.c`.

Each was found the same way: a thread dump with a shadow call stack
(`MKU_TIMEOUT`), then reading the disassembly at the top of the stack.

## Code that isn't in the executable

Entering the first fight killed the game:
`indirect call to unknown address 0x09131E30`. That address is in the heap: the
fight's `.mko` script overlays carry MIPS code, loaded at run time. A static
recompiler can't have seen it. So `runtime/interp.c` is a small interpreter
for just those calls. It shares its semantics with the recompiled code and runs
the same 16 test programs (`tests/recomp/run.py -i`).

## The bug that hid behind two symptoms

After the first fight the game froze; on some stages the fighters were invisible.
Same cause. `func_001DC578` is the process system's **die**: it kills the process
and jumps straight back to the scheduler, so it never returns to its caller.
That jump is `jr $ra` with `$ra` loaded from a global. The recompiler sees "`jr $ra`"
and emits a normal `return`. The dead process carried on running, released its
handles twice, and made a cycle in the free list at 0x30E964: an endless loop.

The clue was in the dump: a process list with garbage, the main thread spinning.
The watchdog now samples what is running when a hang is detected (no new frame
for 15 s), which is what pointed at the list functions. Then searching the code
for every use of the scheduler's saved `$sp`/`$ra` found the one unpatched
function.

**Lesson:** after patching a hand-written assembly system, grep for every
reference to its globals. "It works for two minutes" proved nothing: an earlier,
partial fix (resuming only what the game's own struct calls suspended) made one
run last 272 s, and the next one hung at 112 s.

## 🧑‍🎓 Exercises

1. Run `MKU_GE_DUMP=5000` and read one frame's draws. Which ones are the fighters?
   (Hint: vertex types with bone weights.)
2. In `sas.c`, reverb is missing. Read PPSSPP's `Core/HW/SasReverb.cpp` and add it.
3. Temporarily remove `0x1DC578 proc_die` from `config/patches.txt`, rebuild, and
   watch the hang report. Find the cycle yourself in `build/crash/ram.bin`.
